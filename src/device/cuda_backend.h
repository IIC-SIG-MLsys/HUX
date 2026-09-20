/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_DEVICE_CUDA_BACKEND_H
#define HUX_DEVICE_CUDA_BACKEND_H

#include <map>
#include <mutex>
#include <string>

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

  Status copy(void* dst, void const* src, uint64_t bytes) override;
  Status copy_nowait(void* dst, void const* src, uint64_t bytes) override;
  Status settle() override;
  Status export_ipc(void* addr, uint64_t length, IpcHandle* out) override;
  Status import_ipc(IpcHandle const& handle, void** out) override;
  Status close_ipc(void* mapped) override;

 private:
  /* One mapping per allocation, reference counted. CUDA refuses a second open
   * of the same handle in one process, so two regions inside one peer
   * allocation have to share a mapping rather than each taking their own --
   * and the mapping has to outlive whichever of them is released first. */
  struct Import {
    void* base = nullptr;
    uint64_t bytes = 0;
    std::string handle;
    uint64_t refs = 0;
  };

  int device_index_ = 0;
  mutable std::mutex ipc_mu_;
  std::map<std::string, Import> imports_by_handle_;
  /* Ordered by base address so a span address can be traced back to the
   * mapping that contains it: the caller is given the span it asked for and
   * cannot be expected to remember where the allocation started. */
  std::map<uintptr_t, std::string> imports_by_base_;
};

}  // namespace hux
#endif  // HUX_DEVICE_CUDA_BACKEND_H
