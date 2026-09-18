/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_REQUEST_H
#define HUX_REQUEST_H

#include <cstdint>
#include <memory>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

class DeviceStream;

/* Completion stages. A CQE proves none of these on its own: it shows neither
 * that the data is visible to kernels on the target device, nor that the other
 * QPs of the same logical request are done. Each stage must be provable
 * separately. */
enum class Stage : uint8_t {
  kAccepted = 0,     /* Admitted to the engine. No data has moved. */
  kSourceReusable,   /* Write only: the NIC no longer reads the source. */
  kTransferComplete, /* Provider-level completion. Not yet target_ready. */
  kTargetReady,      /* Data complete and visible; consumers may be ordered. */
  kFailedSafe,       /* Failed; local DMA has stopped or is isolated. */
  kCancelledSafe, /* Cancelled and drained; no success ready was published. */
};

char const* to_string(Stage s);

/* QUEUED -> WAIT_DEPENDENCY -> INFLIGHT -> WAIT_TARGET_READY -> SUCCEEDED,
 * with WAIT_NOTIFY_ACK inserted when a notification is attached. Errors and
 * accepted cancellations pass through DRAINING first. */
enum class RequestState : uint8_t {
  kQueued = 0,
  kWaitDependency,
  kInflight,
  kWaitTargetReady,
  kWaitNotifyAck,
  kDraining,
  kSucceeded,
  kFailed,
  kCancelled,
};

char const* to_string(RequestState s);
bool is_terminal(RequestState s);

/* One logical transfer, possibly spanning many segments, WRs and QPs -- all of
 * which stay internal. The request holds references to the regions and the
 * connection it uses until they are safe to release. */
class Request {
 public:
  virtual ~Request() = default;

  virtual RequestId id() const = 0;
  virtual RequestState state() const = 0;

  /* Repeated queries return consistent results. */
  virtual bool reached(Stage s) const = 0;
  virtual Status test(bool* done) = 0;

  /* A timeout only ends this wait. It does not cancel the request, release the
   * registration, or prove that DMA has stopped. */
  virtual Status wait(int64_t timeout_ms) = 0;

  /* Stops sub-operations not yet posted; posted ones keep draining. Returns
   * kInvalidArgument if the ready handoff is already irrevocable. Remote data
   * is not rolled back. */
  virtual Status cancel() = 0;

  /* Installs the dependency on the stream and returns. Does not block the
   * calling thread on the network, and never synchronizes the whole device. */
  virtual Status wait_on(DeviceStream* stream) = 0;
  virtual Status wait_source_reusable_on(DeviceStream* stream) = 0;

  virtual ErrorInfo const& error() const = 0;

  /* Local correlation only; never sent to the peer. */
  virtual void* context() const = 0;
};

using RequestPtr = std::shared_ptr<Request>;

}  // namespace hux
#endif  // HUX_REQUEST_H
