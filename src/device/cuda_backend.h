/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_DEVICE_CUDA_BACKEND_H
#define HUX_DEVICE_CUDA_BACKEND_H

#include "hux/device.h"

namespace hux {

/* NVIDIA backend. Wraps cudaStream_t and cudaEvent_t so that no CUDA type
 * reaches a public header. */
class CudaBackend : public DeviceBackend {
 public:
  /* Returns kDeviceError if the runtime is unusable, so a caller never gets a
   * backend that looks alive but cannot allocate. */
  static Status create(int device_index, std::shared_ptr<DeviceBackend>* out);

  explicit CudaBackend(int device_index) : device_index_(device_index) {}

  DeviceKind kind() const override { return DeviceKind::kCuda; }
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
#endif  // HUX_DEVICE_CUDA_BACKEND_H
