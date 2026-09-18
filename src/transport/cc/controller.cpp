/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "transport/cc/controller.h"

#include <atomic>
#include <mutex>

namespace hux {
namespace {

class OffController : public CongestionController {
 public:
  char const* name() const override { return "off"; }
  CcVerdict allow(CcDirection, uint64_t, CcTime) override {
    return CcVerdict::kAllowed;
  }
  void on_post(CcDirection dir, uint64_t bytes, CcTime) override {
    inflight_[idx(dir)].fetch_add(bytes, std::memory_order_relaxed);
  }
  void on_feedback(CcDirection dir, uint64_t bytes, std::chrono::nanoseconds,
                   CcTime) override {
    release(dir, bytes);
  }
  void on_error(CcDirection dir, uint64_t bytes, CcTime) override {
    release(dir, bytes);
  }
  CcTime next_send_time(CcDirection) const override { return CcClock::now(); }
  uint64_t inflight_bytes(CcDirection dir) const override {
    return inflight_[idx(dir)].load(std::memory_order_relaxed);
  }
  uint64_t window_bytes(CcDirection) const override { return 0; }

 private:
  static size_t idx(CcDirection d) { return static_cast<size_t>(d); }
  void release(CcDirection dir, uint64_t bytes) {
    /* Clamped rather than allowed to wrap: a release larger than what is
     * outstanding would turn the counter into a huge number and the window
     * would never close again. */
    auto& cell = inflight_[idx(dir)];
    uint64_t cur = cell.load(std::memory_order_relaxed);
    while (true) {
      uint64_t dec = bytes < cur ? bytes : cur;
      if (dec == 0) return;
      if (cell.compare_exchange_weak(cur, cur - dec, std::memory_order_relaxed))
        return;
    }
  }
  std::atomic<uint64_t> inflight_[2]{{0}, {0}};
};

class FixedWindowController : public CongestionController {
 public:
  explicit FixedWindowController(uint64_t window) : window_(window) {}

  char const* name() const override { return "fixed_window"; }

  CcVerdict allow(CcDirection dir, uint64_t bytes, CcTime) override {
    std::lock_guard<std::mutex> g(mu_);
    uint64_t& used = inflight_[idx(dir)];
    if (used + bytes <= window_) return CcVerdict::kAllowed;
    /* A single operation larger than the whole window would otherwise never
     * be allowed, and the transfer would stall for good. It goes out alone,
     * once nothing else is outstanding. */
    if (bytes > window_ && used == 0) return CcVerdict::kAllowed;
    return CcVerdict::kOverBudget;
  }

  void on_post(CcDirection dir, uint64_t bytes, CcTime) override {
    std::lock_guard<std::mutex> g(mu_);
    inflight_[idx(dir)] += bytes;
  }

  void on_feedback(CcDirection dir, uint64_t bytes,
                   std::chrono::nanoseconds rtt, CcTime) override {
    std::lock_guard<std::mutex> g(mu_);
    release_locked(dir, bytes);
    ++samples_[idx(dir)];
    last_rtt_[idx(dir)] = rtt;
  }

  void on_error(CcDirection dir, uint64_t bytes, CcTime) override {
    std::lock_guard<std::mutex> g(mu_);
    release_locked(dir, bytes);
  }

  CcTime next_send_time(CcDirection) const override { return CcClock::now(); }

  uint64_t inflight_bytes(CcDirection dir) const override {
    std::lock_guard<std::mutex> g(mu_);
    return inflight_[idx(dir)];
  }
  uint64_t window_bytes(CcDirection) const override { return window_; }

 private:
  static size_t idx(CcDirection d) { return static_cast<size_t>(d); }
  void release_locked(CcDirection dir, uint64_t bytes) {
    uint64_t& used = inflight_[idx(dir)];
    used -= bytes < used ? bytes : used;
  }

  mutable std::mutex mu_;
  uint64_t const window_;
  uint64_t inflight_[2] = {0, 0};
  uint64_t samples_[2] = {0, 0};
  std::chrono::nanoseconds last_rtt_[2] = {std::chrono::nanoseconds::zero(),
                                           std::chrono::nanoseconds::zero()};
};

/* TIMELY, as a rate with a matching window.
 *
 * Pacing alone would let a burst of small operations run ahead of the rate
 * between two clock reads, so the rate also implies a window -- rate times
 * the smallest delay seen -- and both have to allow an operation before it
 * goes out. */
class TimelyController : public CongestionController {
 public:
  explicit TimelyController(TimelyParams const& p) : p_(p) {
    for (auto& d : dir_) {
      d.rate_bps = p.additive_increase_bps * 10;
      d.min_delay = std::chrono::nanoseconds::max();
      d.next_send = CcClock::now();
    }
  }

  char const* name() const override { return "timely"; }

  CcVerdict allow(CcDirection dir, uint64_t bytes, CcTime now) override {
    std::lock_guard<std::mutex> g(mu_);
    Dir& d = dir_[idx(dir)];
    if (now < d.next_send) return CcVerdict::kPaced;
    uint64_t const window = window_locked(d);
    if (d.inflight + bytes <= window) return CcVerdict::kAllowed;
    /* An operation larger than the whole window would never be allowed and
     * the transfer would stall for good; it goes out alone. */
    if (bytes > window && d.inflight == 0) return CcVerdict::kAllowed;
    return CcVerdict::kOverBudget;
  }

  void on_post(CcDirection dir, uint64_t bytes, CcTime now) override {
    std::lock_guard<std::mutex> g(mu_);
    Dir& d = dir_[idx(dir)];
    d.inflight += bytes;
    /* The doorbell is what gets paced, not a decision taken earlier: the next
     * send time advances by however long these bytes take at the current
     * rate. */
    double const seconds = static_cast<double>(bytes) / d.rate_bps;
    auto const gap = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(seconds));
    CcTime const base = now > d.next_send ? now : d.next_send;
    d.next_send = base + gap;
  }

  void on_feedback(CcDirection dir, uint64_t bytes,
                   std::chrono::nanoseconds rtt, CcTime) override {
    std::lock_guard<std::mutex> g(mu_);
    Dir& d = dir_[idx(dir)];
    release_locked(d, bytes);
    update_rate_locked(d, rtt);
  }

  void on_error(CcDirection dir, uint64_t bytes, CcTime) override {
    std::lock_guard<std::mutex> g(mu_);
    /* Budget is released, but the rate is left alone: a failed operation says
     * nothing about congestion, and treating it as a delay signal would let
     * an unrelated error throttle a healthy path. */
    release_locked(dir_[idx(dir)], bytes);
  }

  CcTime next_send_time(CcDirection dir) const override {
    std::lock_guard<std::mutex> g(mu_);
    return dir_[idx(dir)].next_send;
  }

  uint64_t inflight_bytes(CcDirection dir) const override {
    std::lock_guard<std::mutex> g(mu_);
    return dir_[idx(dir)].inflight;
  }

  uint64_t window_bytes(CcDirection dir) const override {
    std::lock_guard<std::mutex> g(mu_);
    return window_locked(dir_[idx(dir)]);
  }

  double rate_bytes_per_sec(CcDirection dir) const override {
    std::lock_guard<std::mutex> g(mu_);
    return dir_[idx(dir)].rate_bps;
  }

 private:
  struct Dir {
    uint64_t inflight = 0;
    double rate_bps = 0;
    std::chrono::nanoseconds min_delay{};
    std::chrono::nanoseconds prev_delay{};
    double avg_delay_diff_us = 0;
    bool have_prev = false;
    CcTime next_send{};
  };

  static size_t idx(CcDirection d) { return static_cast<size_t>(d); }

  static void release_locked(Dir& d, uint64_t bytes) {
    d.inflight -= bytes < d.inflight ? bytes : d.inflight;
  }

  uint64_t window_locked(Dir const& d) const {
    /* Rate times the smallest delay observed: what can be in flight without
     * the rate being exceeded. Floored at one maximum-size operation so the
     * window can never close entirely. */
    double base_ns = d.min_delay == std::chrono::nanoseconds::max()
                         ? 1e6 /* no sample yet: assume a millisecond */
                         : static_cast<double>(d.min_delay.count());
    double bytes = d.rate_bps * base_ns / 1e9;
    uint64_t w = static_cast<uint64_t>(bytes);
    return w < (1u << 16) ? (1u << 16) : w;
  }

  void update_rate_locked(Dir& d, std::chrono::nanoseconds rtt) {
    if (rtt <= std::chrono::nanoseconds::zero()) return;
    if (rtt < d.min_delay) d.min_delay = rtt;

    double const sample_us = static_cast<double>(rtt.count()) / 1000.0;
    double const t_low_us = static_cast<double>(p_.t_low.count());
    double const t_high_us = static_cast<double>(p_.t_high.count());

    if (!d.have_prev) {
      d.prev_delay = rtt;
      d.have_prev = true;
      return;
    }
    double const diff_us =
        static_cast<double>((rtt - d.prev_delay).count()) / 1000.0;
    d.prev_delay = rtt;
    d.avg_delay_diff_us =
        (1.0 - p_.ewma_alpha) * d.avg_delay_diff_us + p_.ewma_alpha * diff_us;

    double new_rate = d.rate_bps;
    if (sample_us < t_low_us) {
      /* Below the low threshold there is no queue worth reacting to, so the
       * gradient is ignored and the rate simply grows. */
      new_rate += p_.additive_increase_bps;
    } else if (sample_us > t_high_us) {
      /* A queue has already built; back off in proportion to the overshoot. */
      new_rate *= 1.0 - p_.beta * (1.0 - t_high_us / sample_us);
    } else {
      double const min_us = static_cast<double>(d.min_delay.count()) / 1000.0;
      double const norm_grad =
          d.avg_delay_diff_us / (min_us > 0 ? min_us : 1.0);
      if (norm_grad <= 0) {
        new_rate += p_.additive_increase_bps;
      } else {
        /* Delay is rising while still within bounds: slow down before the
         * queue grows, which is the whole point of reacting to the gradient
         * rather than waiting for loss. */
        new_rate *= 1.0 - p_.beta * norm_grad;
      }
    }

    if (new_rate < p_.min_rate_bps) new_rate = p_.min_rate_bps;
    if (new_rate > p_.max_rate_bps) new_rate = p_.max_rate_bps;
    d.rate_bps = new_rate;
  }

  mutable std::mutex mu_;
  TimelyParams const p_;
  Dir dir_[2];
};

}  // namespace

CongestionControllerPtr make_cc_timely(TimelyParams const& params) {
  return std::make_shared<TimelyController>(params);
}

CongestionControllerPtr make_cc_off() {
  return std::make_shared<OffController>();
}

CongestionControllerPtr make_cc_fixed_window(uint64_t window_bytes) {
  return std::make_shared<FixedWindowController>(window_bytes);
}

}  // namespace hux
