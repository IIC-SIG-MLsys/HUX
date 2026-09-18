/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_DEVICE_HOST_BACKEND_H
#define HUX_DEVICE_HOST_BACKEND_H

#include "hux/device.h"

namespace hux {

/* Host memory backend. It has no execution queue, so stream and event calls
 * report kUnsupported rather than pretending to order anything: a caller that
 * needs GPU ordering must see that this backend cannot provide it. */
class HostBackend : public DeviceBackend {
 public:
  DeviceKind kind() const override { return DeviceKind::kHost; }
  DeviceCaps caps() const override;

  Status probe_pointer(void const* ptr, DeviceId* dev,
                       MemoryKind* mem) const override;
  Status import_stream(void* native_stream, DeviceStreamPtr* out) override;
  Status record_event(DeviceStream* stream, DeviceEventPtr* out) override;
  Status stream_wait_event(DeviceStream* stream, DeviceEvent* ev) override;
  Status make_visible(DeviceStream* stream, void* addr,
                      uint64_t bytes) override;
};

}  // namespace hux
#endif  // HUX_DEVICE_HOST_BACKEND_H
