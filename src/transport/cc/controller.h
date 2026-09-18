/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Congestion control, as a replaceable policy.
 *
 * The interface is deliberately about bytes and time rather than work
 * requests: queue depth is a hardware limit, not a network window, and
 * treating one as the other is how a transport comes to believe it is pacing
 * when it is only counting.
 *
 * Read and write are budgeted separately. A write sender governs traffic it
 * emits; a read initiator governs what it asks others to send, and the bytes
 * leave a NIC it does not control. Sharing one budget between them would
 * claim an authority this side does not have. */
#ifndef HUX_TRANSPORT_CC_CONTROLLER_H
#define HUX_TRANSPORT_CC_CONTROLLER_H

#include <chrono>
#include <cstdint>
#include <memory>

namespace hux {

using CcClock = std::chrono::steady_clock;
using CcTime = CcClock::time_point;

enum class CcDirection : uint8_t { kWrite = 0, kRead = 1 };

/* Why a post was refused, so a caller can tell "come back later" from
 * "never". */
enum class CcVerdict : uint8_t {
  kAllowed = 0,
  kOverBudget, /* in-flight bytes would exceed the window */
  kPaced,      /* allowed by budget, but not until next_send_time */
};

class CongestionController {
 public:
  virtual ~CongestionController() = default;
  virtual char const* name() const = 0;

  /* Asked before every post, never after. Pacing that is applied to a
   * decision already taken is not pacing. */
  virtual CcVerdict allow(CcDirection dir, uint64_t bytes, CcTime now) = 0;

  /* A work request actually reached the queue. */
  virtual void on_post(CcDirection dir, uint64_t bytes, CcTime now) = 0;

  /* A completion arrived. rtt is measured from the post, so it includes
   * serialization, queueing at the NIC and host, and polling delay -- it is
   * not a pure network round trip and must not be reported as one. */
  virtual void on_feedback(CcDirection dir, uint64_t bytes,
                           std::chrono::nanoseconds rtt, CcTime now) = 0;

  /* A completion failed. Budget is released exactly once either way, so a
   * failure cannot leak window. */
  virtual void on_error(CcDirection dir, uint64_t bytes, CcTime now) = 0;

  /* Earliest time a paced controller would allow the next post. */
  virtual CcTime next_send_time(CcDirection dir) const = 0;

  virtual uint64_t inflight_bytes(CcDirection dir) const = 0;
  virtual uint64_t window_bytes(CcDirection dir) const = 0;

  /* Current sending rate in bytes per second, or 0 for a controller that does
   * not work in rates. Reported so a run can show what the controller
   * actually did, rather than only which one was configured. */
  virtual double rate_bytes_per_sec(CcDirection) const { return 0.0; }
};

using CongestionControllerPtr = std::shared_ptr<CongestionController>;

/* No limit at all. Kept as the comparison every other configuration is
 * measured against, not as a placeholder. */
CongestionControllerPtr make_cc_off();

/* A fixed byte window per direction. Also the control case for the adaptive
 * controller: without it, an improvement cannot be separated from the effect
 * of simply having a window. */
CongestionControllerPtr make_cc_fixed_window(uint64_t window_bytes);

/* Tunables for the adaptive controller. Defaults follow the TIMELY paper,
 * scaled for a datacentre fabric.
 *
 * The delay these thresholds are compared against is measured from the post
 * to its completion. That covers serialization, queueing at the NIC and the
 * host, and how promptly the completion queue was polled. It behaves like a
 * congestion signal and the algorithm works on it, but it is not a network
 * round trip and results derived from it must not be labelled as one. */
struct TimelyParams {
  std::chrono::microseconds t_low{50};
  std::chrono::microseconds t_high{500};
  /* Multiplicative decrease factor. */
  double beta = 0.008;
  /* Additive increase per round, in bytes per second. */
  double additive_increase_bps = 10e6;
  /* Weight for the delay-difference EWMA; higher tracks recent samples more
   * closely. */
  double ewma_alpha = 0.3;
  double min_rate_bps = 1e6;   /* never paces down to a stop */
  double max_rate_bps = 100e9; /* line rate of the fabric under test */
};

/* Rate-based adaptive control after TIMELY. Reacts to the delay gradient
 * rather than to loss, so it responds before a queue has built up rather than
 * after it has overflowed. */
CongestionControllerPtr make_cc_timely(TimelyParams const& params = {});

}  // namespace hux
#endif  // HUX_TRANSPORT_CC_CONTROLLER_H
