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
