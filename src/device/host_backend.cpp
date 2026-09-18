/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "device/host_backend.h"

namespace hux {

DeviceCaps HostBackend::caps() const {
  DeviceCaps c;
  c.supports_stream = false;
  c.supports_graph_capture = false;
  c.supports_peer_registration = true;  /* Host memory registers directly. */
  c.supports_dmabuf_export = false;
  c.max_registration_bytes = 0;
  return c;
}

Status HostBackend::probe_pointer(void const* ptr, DeviceId* dev,
                                  MemoryKind* mem) const {
  if (ptr == nullptr || dev == nullptr || mem == nullptr)
    return Status::kInvalidArgument;
  dev->kind = DeviceKind::kHost;
  dev->index = 0;
  /* Pinned host memory cannot be told apart from pageable without a vendor
   * runtime, so it is reported as pageable here. A GPU backend that owns the
   * pinning reports it correctly. */
  *mem = MemoryKind::kHostPageable;
  return Status::kOk;
}

Status HostBackend::import_stream(void*, DeviceStreamPtr*) {
  return Status::kUnsupported;
}

Status HostBackend::record_event(DeviceStream*, DeviceEventPtr*) {
  return Status::kUnsupported;
}

Status HostBackend::stream_wait_event(DeviceStream*, DeviceEvent*) {
  return Status::kUnsupported;
}

Status HostBackend::make_visible(DeviceStream*, void*, uint64_t) {
  /* Host writes are visible to host reads once the transport reports
   * completion; there is no device-side visibility step to perform. */
  return Status::kOk;
}

}  // namespace hux
