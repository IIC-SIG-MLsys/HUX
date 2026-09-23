/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include <sstream>
#include <string>

#include "hux/config.h"
#include "hux/engine.h"
#include "hux/notification.h"
#include "hux/peer.h"
#include "hux/request.h"
#include "hux/status.h"

namespace hux {

char const* to_string(Status s) {
  switch (s) {
    case Status::kOk:
      return "ok";
    case Status::kWouldBlock:
      return "would_block";
    case Status::kTimeout:
      return "timeout";
    case Status::kCancelled:
      return "cancelled";
    case Status::kUnsupported:
      return "unsupported";
    case Status::kInvalidArgument:
      return "invalid_argument";
    case Status::kOutOfRange:
      return "out_of_range";
    case Status::kNotFound:
      return "not_found";
    case Status::kStaleGeneration:
      return "stale_generation";
    case Status::kPeerDisconnected:
      return "peer_disconnected";
    case Status::kResourceExhausted:
      return "resource_exhausted";
    case Status::kDeviceError:
      return "device_error";
    case Status::kTransportError:
      return "transport_error";
    case Status::kInternal:
      return "internal";
  }
  return "unknown";
}

char const* to_string(Stage s) {
  switch (s) {
    case Stage::kAccepted:
      return "accepted";
    case Stage::kSourceReusable:
      return "source_reusable";
    case Stage::kTransferComplete:
      return "transfer_complete";
    case Stage::kTargetReady:
      return "target_ready";
    case Stage::kFailedSafe:
      return "failed_safe";
    case Stage::kCancelledSafe:
      return "cancelled_safe";
  }
  return "unknown";
}

char const* to_string(RequestState s) {
  switch (s) {
    case RequestState::kQueued:
      return "queued";
    case RequestState::kWaitDependency:
      return "wait_dependency";
    case RequestState::kInflight:
      return "inflight";
    case RequestState::kWaitTargetReady:
      return "wait_target_ready";
    case RequestState::kWaitNotifyAck:
      return "wait_notify_ack";
    case RequestState::kDraining:
      return "draining";
    case RequestState::kSucceeded:
      return "succeeded";
    case RequestState::kFailed:
      return "failed";
    case RequestState::kCancelled:
      return "cancelled";
  }
  return "unknown";
}

bool is_terminal(RequestState s) {
  return s == RequestState::kSucceeded || s == RequestState::kFailed ||
         s == RequestState::kCancelled;
}

char const* to_string(DeliveryState s) {
  switch (s) {
    case DeliveryState::kPending:
      return "pending";
    case DeliveryState::kDelivered:
      return "delivered";
    case DeliveryState::kFailed:
      return "failed";
    case DeliveryState::kIndeterminate:
      return "indeterminate";
  }
  return "unknown";
}

char const* to_string(PathKind p) {
  switch (p) {
    case PathKind::kUnknown:
      return "unknown";
    case PathKind::kSameProcess:
      return "same_process";
    case PathKind::kIpc:
      return "ipc";
    case PathKind::kRdma:
      return "rdma";
    case PathKind::kUcx:
      return "ucx";
  }
  return "unknown";
}

char const* to_string(PeerPlace p) {
  switch (p) {
    case PeerPlace::kSameProcess:
      return "same_process";
    case PeerPlace::kSameHost:
      return "same_host";
    case PeerPlace::kAnotherHost:
      return "another_host";
    case PeerPlace::kUnknown:
      break;
  }
  return "unknown";
}

namespace {

char const* progress_name(ProgressMode m) {
  return m == ProgressMode::kThread ? "thread" : "explicit";
}

char const* cc_name(CongestionControl c) {
  switch (c) {
    case CongestionControl::kOff:
      return "off";
    case CongestionControl::kFixedWindow:
      return "fixed_window";
    case CongestionControl::kAdaptive:
      return "adaptive";
  }
  return "unknown";
}

char const* device_name(DeviceKind k) {
  switch (k) {
    case DeviceKind::kHost:
      return "host";
    case DeviceKind::kCuda:
      return "cuda";
    case DeviceKind::kRocm:
      return "rocm";
    case DeviceKind::kCambricon:
      return "cambricon";
    case DeviceKind::kMoore:
      return "moore";
  }
  return "unknown";
}

}  // namespace

std::string describe_config(EngineConfig const& cfg) {
  std::ostringstream o;
  o << "{"
    << "\"device\":\"" << device_name(cfg.device.kind) << ':'
    << cfg.device.index << "\","
    << "\"progress\":\"" << progress_name(cfg.progress) << "\","
    << "\"qp_per_peer\":" << cfg.qp_per_peer << ','
    << "\"chunk_bytes\":" << cfg.chunk_bytes << ','
    << "\"wr_batch\":" << cfg.wr_batch << ',' << "\"cq_batch\":" << cfg.cq_batch
    << ',' << "\"max_inflight_requests\":" << cfg.max_inflight_requests << ','
    << "\"scheduler_quantum_bytes\":" << cfg.scheduler_quantum_bytes << ','
    << "\"registration_cache_entries\":" << cfg.registration_cache_entries
    << ',' << "\"cc\":\"" << cc_name(cfg.cc) << "\","
    << "\"cc_window_bytes\":" << cfg.cc_window_bytes << ','
    << "\"notify_queue_depth\":" << cfg.notify_queue_depth << ','
    << "\"completion_queue_depth\":" << cfg.completion_queue_depth << ','
    << "\"ready_queue_depth\":" << cfg.ready_queue_depth << ','
    << "\"notify_max_payload\":" << cfg.notify_max_payload << ','
    << "\"preferred_provider\":\"" << cfg.preferred_provider << "\""
    << "}";
  return o.str();
}

/* Conflicting parameters are rejected with a reason. Silently rewriting them
 * would leave the caller believing it runs a configuration it does not. */
Status EngineConfig::validate(std::string* reason) const {
  auto fail = [&](char const* why) {
    if (reason != nullptr) *reason = why;
    return Status::kInvalidArgument;
  };
  if (qp_per_peer == 0) return fail("qp_per_peer must be >= 1");
  if (chunk_bytes == 0) return fail("chunk_bytes must be > 0");
  /* A work request carries a 32-bit length: a 4 GiB chunk went out as zero
   * bytes, and was reported as having moved. */
  if (chunk_bytes > 0xffffffffull)
    return fail("chunk_bytes must fit in 32 bits");
  if (wr_batch == 0) return fail("wr_batch must be >= 1");
  if (cq_batch == 0) return fail("cq_batch must be >= 1");
  if (max_inflight_requests == 0)
    return fail("max_inflight_requests must be >= 1");
  if (notify_queue_depth == 0) return fail("notify_queue_depth must be >= 1");
  if (notify_max_payload == 0) return fail("notify_max_payload must be > 0");
  if (cc == CongestionControl::kFixedWindow && cc_window_bytes == 0)
    return fail("cc_window_bytes must be > 0 when cc=fixed_window");
  if (cc == CongestionControl::kFixedWindow && cc_window_bytes < chunk_bytes)
    return fail("cc_window_bytes < chunk_bytes would stall every request");
  if (reason != nullptr) reason->clear();
  return Status::kOk;
}

}  // namespace hux
