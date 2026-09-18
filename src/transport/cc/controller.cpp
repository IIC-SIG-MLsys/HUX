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

  void on_feedback(CcDirection dir, uint64_t bytes, std::chrono::nanoseconds rtt,
                   CcTime) override {
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

}  // namespace

CongestionControllerPtr make_cc_off() {
  return std::make_shared<OffController>();
}

CongestionControllerPtr make_cc_fixed_window(uint64_t window_bytes) {
  return std::make_shared<FixedWindowController>(window_bytes);
}

}  // namespace hux
