/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_DEVICE_ROCM_BACKEND_H
#define HUX_DEVICE_ROCM_BACKEND_H

#include <map>
#include <mutex>
#include <string>

#include "hux/device.h"

namespace hux {

/* AMD and Hygon DCU share the HIP runtime, so one backend covers both. They
 * differ in capability rather than API: Hygon's DTK ships no
 * hipMemGetHandleForAddressRange, hence no DMA-BUF export. */
class RocmBackend : public DeviceBackend {
 public:
  static Status create(int device_index, std::shared_ptr<DeviceBackend>* out);

  explicit RocmBackend(int device_index) : device_index_(device_index) {}

  DeviceKind kind() const override { return DeviceKind::kRocm; }
  DeviceCaps caps() const override;

  Status probe_pointer(void const* ptr, DeviceId* dev,
                       MemoryKind* mem) const override;
  Status import_stream(void* native_stream, DeviceStreamPtr* out) override;
  Status record_event(DeviceStream* stream, DeviceEventPtr* out) override;
  Status stream_wait_event(DeviceStream* stream, DeviceEvent* ev) override;
  Status make_visible(DeviceStream* stream, void* addr,
                      uint64_t bytes) override;

  Status copy(void* dst, void const* src, uint64_t bytes) override;
  Status export_ipc(void* addr, uint64_t length, IpcHandle* out) override;
  Status import_ipc(IpcHandle const& handle, void** out) override;
  Status close_ipc(void* mapped) override;

 private:
  /* One mapping per allocation, reference counted: HIP, like CUDA, refuses a
   * second open of the same handle in one process, so two regions inside one
   * peer allocation share a mapping and it outlives whichever is released
   * first. */
  struct Import {
    void* base = nullptr;
    uint64_t bytes = 0;
    uint64_t refs = 0;
  };

  /* Answered once, and by the driver when the attribute query cannot answer
   * it -- see the definition. */
  bool ipc_supported() const;

  int device_index_ = 0;
  mutable std::once_flag ipc_probe_;
  mutable bool ipc_supported_ = false;
  mutable std::mutex ipc_mu_;
  std::map<std::string, Import> imports_by_handle_;
  std::map<uintptr_t, std::string> imports_by_base_;
};

}  // namespace hux
#endif  // HUX_DEVICE_ROCM_BACKEND_H
