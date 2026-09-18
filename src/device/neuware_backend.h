/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_DEVICE_NEUWARE_BACKEND_H
#define HUX_DEVICE_NEUWARE_BACKEND_H

#include "hux/device.h"

namespace hux {

/* Cambricon MLU. Two things set it apart from the CUDA-like backends:
 * registration is capped per process, and the allocation call that looks right
 * for peer access is the wrong one -- see caps() and the note in the .cpp. */
class NeuwareBackend : public DeviceBackend {
 public:
  static Status create(int device_index, std::shared_ptr<DeviceBackend>* out);

  explicit NeuwareBackend(int device_index) : device_index_(device_index) {}

  DeviceKind kind() const override { return DeviceKind::kCambricon; }
  DeviceCaps caps() const override;

  Status probe_pointer(void const* ptr, DeviceId* dev,
                       MemoryKind* mem) const override;
  Status import_stream(void* native_stream, DeviceStreamPtr* out) override;
  Status record_event(DeviceStream* stream, DeviceEventPtr* out) override;
  Status stream_wait_event(DeviceStream* stream, DeviceEvent* ev) override;
  Status make_visible(DeviceStream* stream, void* addr,
                      uint64_t bytes) override;

 private:
  int device_index_ = 0;
};

}  // namespace hux
#endif  // HUX_DEVICE_NEUWARE_BACKEND_H
