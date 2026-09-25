/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_DEVICE_KUNLUN_BACKEND_H
#define HUX_DEVICE_KUNLUN_BACKEND_H

#include <map>
#include <mutex>
#include <string>

#include "hux/device.h"

namespace hux {

/* Kunlunxin XPU (P800), through the CUDA-compatible runtime the SDK ships.
 *
 * Not the CUDA backend with a different library underneath: three things
 * measured on XPU 5.18 differ, and each would be a silent fault rather than
 * an error if it were assumed away.
 *
 *   - cudaMemcpyDefault is refused, so every copy states its direction and
 *     this backend works it out from both addresses.
 *   - there is no driver API, so an allocation's base cannot be looked up;
 *     export_ipc establishes whether it was handed one.
 *   - the NIC cannot register device memory at all, so transfers off this
 *     host stage through host memory. caps() says so.
 *
 * The native xpu_* API is not used: its pointer query is declared in the
 * header and exported by no library in the SDK, and without it a backend
 * cannot tell device memory from host memory. */
class KunlunBackend : public DeviceBackend {
 public:
  static Status create(int device_index, std::shared_ptr<DeviceBackend>* out);

  explicit KunlunBackend(int device_index) : device_index_(device_index) {}

  DeviceKind kind() const override { return DeviceKind::kKunlun; }
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
  struct Import {
    void* base = nullptr;
    uint64_t bytes = 0;
    std::string handle;
    uint64_t refs = 0;
  };

  int device_index_ = 0;
  mutable std::mutex ipc_mu_;
  std::map<std::string, Import> imports_by_handle_;
  std::map<uintptr_t, std::string> imports_by_base_;
};

}  // namespace hux
#endif  // HUX_DEVICE_KUNLUN_BACKEND_H
