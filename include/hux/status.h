/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_STATUS_H
#define HUX_STATUS_H

#include <cstdint>
#include <string>

namespace hux {

/* Public error codes. kWouldBlock, kTimeout and kUnsupported are kept apart on
 * purpose: the caller's correct reaction differs for each. */
enum class Status : int32_t {
  kOk = 0,
  kWouldBlock,       /* Not accepted, no network side effect; safe to retry. */
  kTimeout,          /* This wait ended; the request is still in flight. */
  kCancelled,
  kUnsupported,      /* Capability is absent, not a transient failure. */
  kInvalidArgument,
  kOutOfRange,       /* Offset/length outside the region, or integer overflow. */
  kNotFound,         /* Unknown peer, region or request handle. */
  kStaleGeneration,  /* Descriptor generation no longer valid. */
  kPeerDisconnected,
  kResourceExhausted,
  kDeviceError,
  kTransportError,
  kInternal,
};

char const* to_string(Status s);
inline bool ok(Status s) { return s == Status::kOk; }

/* Failure detail. Batch transfers are not atomic, so may_have_modified_target
 * is the only thing a caller can rely on to decide what the remote side holds. */
struct ErrorInfo {
  Status status = Status::kOk;
  std::string provider;
  uint64_t peer_id = 0;
  int32_t provider_errno = 0;
  bool may_have_modified_target = false;
  std::string detail;

  bool ok() const { return status == Status::kOk; }
};

}  // namespace hux
#endif  // HUX_STATUS_H
