/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Congestion control boundaries. These are the cases where a window either
 * leaks, deadlocks, or claims an authority the sender does not have -- none
 * of which shows up in a throughput number. */
#include <chrono>
#include <string>

#include "test_main.h"
#include "transport/cc/controller.h"

using namespace hux;

namespace {
constexpr uint64_t kWindow = 1u << 20; /* 1 MiB */
CcTime now() { return CcClock::now(); }
}  // namespace

HUX_TEST(cc_off_never_refuses) {
  auto cc = make_cc_off();
  CHECK(std::string(cc->name()) == "off");
  for (int i = 0; i < 1000; ++i) {
    CHECK(cc->allow(CcDirection::kWrite, 1u << 20, now()) ==
          CcVerdict::kAllowed);
    cc->on_post(CcDirection::kWrite, 1u << 20, now());
  }
  /* It still accounts for what is in flight -- "off" means unlimited, not
   * unmeasured, or a comparison against it would have nothing to compare. */
  CHECK_EQ(cc->inflight_bytes(CcDirection::kWrite), 1000ull << 20);
}

HUX_TEST(fixed_window_refuses_past_its_limit) {
  auto cc = make_cc_fixed_window(kWindow);
  CHECK(cc->allow(CcDirection::kWrite, kWindow / 2, now()) ==
        CcVerdict::kAllowed);
  cc->on_post(CcDirection::kWrite, kWindow / 2, now());
  CHECK(cc->allow(CcDirection::kWrite, kWindow / 2, now()) ==
        CcVerdict::kAllowed);
  cc->on_post(CcDirection::kWrite, kWindow / 2, now());

  /* Full. */
  CHECK(cc->allow(CcDirection::kWrite, 1, now()) == CcVerdict::kOverBudget);
  CHECK_EQ(cc->inflight_bytes(CcDirection::kWrite), kWindow);
}

HUX_TEST(fixed_window_reopens_on_completion) {
  auto cc = make_cc_fixed_window(kWindow);
  cc->on_post(CcDirection::kWrite, kWindow, now());
  CHECK(cc->allow(CcDirection::kWrite, 1, now()) == CcVerdict::kOverBudget);

  cc->on_feedback(CcDirection::kWrite, kWindow / 2,
                  std::chrono::nanoseconds(1000), now());
  CHECK(cc->allow(CcDirection::kWrite, kWindow / 2, now()) ==
        CcVerdict::kAllowed);
  CHECK_EQ(cc->inflight_bytes(CcDirection::kWrite), kWindow / 2);
}

HUX_TEST(a_failure_releases_budget_too) {
  /* A failure that leaked window would shrink the effective limit a little
   * more each time, until the transport quietly stopped sending. */
  auto cc = make_cc_fixed_window(kWindow);
  cc->on_post(CcDirection::kWrite, kWindow, now());
  cc->on_error(CcDirection::kWrite, kWindow, now());
  CHECK_EQ(cc->inflight_bytes(CcDirection::kWrite), 0u);
  CHECK(cc->allow(CcDirection::kWrite, kWindow, now()) == CcVerdict::kAllowed);
}

HUX_TEST(an_operation_larger_than_the_window_still_goes_out) {
  /* Otherwise it could never be allowed and the transfer would stall for
   * good. It goes alone, once nothing else is outstanding. */
  auto cc = make_cc_fixed_window(kWindow);
  CHECK(cc->allow(CcDirection::kWrite, kWindow * 4, now()) ==
        CcVerdict::kAllowed);
  cc->on_post(CcDirection::kWrite, kWindow * 4, now());
  /* While it is in flight nothing else may join it. */
  CHECK(cc->allow(CcDirection::kWrite, 1, now()) == CcVerdict::kOverBudget);
}

HUX_TEST(read_and_write_are_budgeted_separately) {
  /* A write sender governs traffic it emits. A read initiator governs what it
   * asks others to send, and those bytes leave a NIC it does not control.
   * One shared budget would claim an authority this side does not have. */
  auto cc = make_cc_fixed_window(kWindow);
  cc->on_post(CcDirection::kWrite, kWindow, now());

  CHECK(cc->allow(CcDirection::kWrite, 1, now()) == CcVerdict::kOverBudget);
  CHECK(cc->allow(CcDirection::kRead, kWindow, now()) == CcVerdict::kAllowed);
  CHECK_EQ(cc->inflight_bytes(CcDirection::kRead), 0u);
}

HUX_TEST(releasing_more_than_is_outstanding_does_not_wrap) {
  /* An unclamped subtraction would turn the counter into a huge number and
   * the window would never close again. */
  auto cc = make_cc_fixed_window(kWindow);
  cc->on_post(CcDirection::kWrite, 1024, now());
  cc->on_feedback(CcDirection::kWrite, 4096, std::chrono::nanoseconds(1),
                  now());
  CHECK_EQ(cc->inflight_bytes(CcDirection::kWrite), 0u);
  CHECK(cc->allow(CcDirection::kWrite, kWindow, now()) == CcVerdict::kAllowed);

  auto off = make_cc_off();
  off->on_post(CcDirection::kRead, 1024, now());
  off->on_error(CcDirection::kRead, 999999, now());
  CHECK_EQ(off->inflight_bytes(CcDirection::kRead), 0u);
}

HUX_TEST(window_is_reported_so_a_run_can_be_identified) {
  /* Comparing configurations means knowing which one produced a result. */
  auto off = make_cc_off();
  auto fixed = make_cc_fixed_window(kWindow);
  CHECK_EQ(off->window_bytes(CcDirection::kWrite), 0u);
  CHECK_EQ(fixed->window_bytes(CcDirection::kWrite), kWindow);
  CHECK(std::string(fixed->name()) == "fixed_window");
}

/* ---- Adaptive control ---- */

namespace {

/* Feeds the controller a delay sample as if an operation of that size had
 * completed, so the rate can be driven deterministically. */
void feed(CongestionControllerPtr const& cc, uint64_t bytes, int micros) {
  cc->on_post(CcDirection::kWrite, bytes, now());
  cc->on_feedback(CcDirection::kWrite, bytes, std::chrono::microseconds(micros),
                  now());
}

}  // namespace

HUX_TEST(timely_speeds_up_while_delay_stays_low) {
  TimelyParams p;
  p.t_low = std::chrono::microseconds(50);
  auto cc = make_cc_timely(p);
  CHECK(std::string(cc->name()) == "timely");

  double const start = cc->rate_bytes_per_sec(CcDirection::kWrite);
  for (int i = 0; i < 20; ++i) feed(cc, 4096, 10); /* well under t_low */
  double const end = cc->rate_bytes_per_sec(CcDirection::kWrite);
  std::printf("       low delay: %.1f -> %.1f MB/s\n", start / 1e6, end / 1e6);
  CHECK(end > start);
}

HUX_TEST(timely_backs_off_when_delay_exceeds_the_high_threshold) {
  TimelyParams p;
  p.t_low = std::chrono::microseconds(50);
  p.t_high = std::chrono::microseconds(200);
  /* A sharper factor than the default, so a handful of samples is enough to
   * show the direction without needing hundreds. */
  p.beta = 0.5;
  auto cc = make_cc_timely(p);

  for (int i = 0; i < 20; ++i) feed(cc, 4096, 10);
  double const fast = cc->rate_bytes_per_sec(CcDirection::kWrite);

  for (int i = 0; i < 20; ++i) feed(cc, 4096, 5000); /* far above t_high */
  double const slow = cc->rate_bytes_per_sec(CcDirection::kWrite);

  std::printf("       high delay: %.1f -> %.1f MB/s\n", fast / 1e6, slow / 1e6);
  CHECK(slow < fast);
}

HUX_TEST(timely_slows_on_a_rising_gradient_before_the_threshold) {
  /* The point of reacting to the gradient: delay is still within bounds, but
   * climbing, so the rate comes down before a queue has formed rather than
   * after it overflows. */
  TimelyParams p;
  p.t_low = std::chrono::microseconds(10);
  p.t_high = std::chrono::microseconds(10000);
  p.beta = 0.5;
  p.additive_increase_bps = 1e6;
  auto cc = make_cc_timely(p);

  /* Settle at a steady delay between the thresholds. */
  for (int i = 0; i < 10; ++i) feed(cc, 4096, 100);
  double const steady = cc->rate_bytes_per_sec(CcDirection::kWrite);

  /* Now let it climb, still far below t_high. */
  for (int i = 1; i <= 20; ++i) feed(cc, 4096, 100 + i * 50);
  double const rising = cc->rate_bytes_per_sec(CcDirection::kWrite);

  std::printf("       rising gradient: %.1f -> %.1f MB/s\n", steady / 1e6,
              rising / 1e6);
  CHECK(rising < steady);
}

HUX_TEST(timely_rate_stays_within_its_bounds) {
  TimelyParams p;
  p.min_rate_bps = 1e6;
  p.max_rate_bps = 10e6;
  p.t_low = std::chrono::microseconds(50);
  p.t_high = std::chrono::microseconds(100);
  p.beta = 0.9;
  auto cc = make_cc_timely(p);

  for (int i = 0; i < 200; ++i) feed(cc, 4096, 1);
  CHECK(cc->rate_bytes_per_sec(CcDirection::kWrite) <= p.max_rate_bps);

  for (int i = 0; i < 500; ++i) feed(cc, 4096, 100000);
  /* Never paced down to a stop: a rate of zero would never recover, since
   * recovery depends on completions that can no longer be sent. */
  CHECK(cc->rate_bytes_per_sec(CcDirection::kWrite) >= p.min_rate_bps);
}

HUX_TEST(timely_does_not_treat_a_failure_as_congestion) {
  /* A failed operation says nothing about queueing. Throttling on it would
   * let an unrelated error slow a healthy path. */
  auto cc = make_cc_timely();
  for (int i = 0; i < 10; ++i) feed(cc, 4096, 10);
  double const before = cc->rate_bytes_per_sec(CcDirection::kWrite);

  cc->on_post(CcDirection::kWrite, 4096, now());
  cc->on_error(CcDirection::kWrite, 4096, now());

  CHECK_EQ(cc->rate_bytes_per_sec(CcDirection::kWrite), before);
  CHECK_EQ(cc->inflight_bytes(CcDirection::kWrite), 0u);
}

HUX_TEST(timely_paces_the_doorbell) {
  /* Pacing has to constrain the actual submission. A next send time that
   * never moves would mean the rate exists only on paper. */
  TimelyParams p;
  p.max_rate_bps = 1e6; /* 1 MB/s: 64 KiB takes about 65 ms */
  auto cc = make_cc_timely(p);

  CcTime const before = cc->next_send_time(CcDirection::kWrite);
  cc->on_post(CcDirection::kWrite, 64 << 10, now());
  CcTime const after = cc->next_send_time(CcDirection::kWrite);
  CHECK(after > before);

  /* And it refuses until that time arrives. */
  CHECK(cc->allow(CcDirection::kWrite, 1, now()) == CcVerdict::kPaced);
}
