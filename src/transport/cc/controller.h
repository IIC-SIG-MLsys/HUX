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
};

using CongestionControllerPtr = std::shared_ptr<CongestionController>;

/* No limit at all. Kept as the comparison every other configuration is
 * measured against, not as a placeholder. */
CongestionControllerPtr make_cc_off();

/* A fixed byte window per direction. Also the control case for the adaptive
 * controller: without it, an improvement cannot be separated from the effect
 * of simply having a window. */
CongestionControllerPtr make_cc_fixed_window(uint64_t window_bytes);

}  // namespace hux
#endif  // HUX_TRANSPORT_CC_CONTROLLER_H
