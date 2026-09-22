/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "device/rocm_backend.h"

#include <hip/hip_runtime.h>

#include <cstring>

namespace hux {
namespace {

class RocmStream : public DeviceStream {
 public:
  RocmStream(hipStream_t s, DeviceId d) : stream_(s), dev_(d) {}
  DeviceId device() const override { return dev_; }
  void* native_handle() const override { return stream_; }
  hipStream_t raw() const { return stream_; }

 private:
  hipStream_t stream_;
  DeviceId dev_;
};

/* HIP will report success for an event that was never recorded, where CUDA
 * reports not-ready. Tracking recorded state here keeps both vendors under one
 * rule, so a caller cannot build a dependency on an event holding no work. */
class RocmEvent : public DeviceEvent {
 public:
  RocmEvent(hipEvent_t e, DeviceId d) : event_(e), dev_(d) {}
  ~RocmEvent() override {
    if (event_ != nullptr) (void)hipEventDestroy(event_);
  }

  DeviceId device() const override { return dev_; }
  bool recorded() const override { return recorded_; }
  void* native_handle() const override { return event_; }

  Status query(bool* complete) override {
    if (complete == nullptr) return Status::kInvalidArgument;
    if (!recorded_) {
      *complete = false;
      return Status::kInvalidArgument;
    }
    hipError_t e = hipEventQuery(event_);
    if (e == hipSuccess) {
      *complete = true;
      return Status::kOk;
    }
    if (e == hipErrorNotReady) {
      *complete = false;
      return Status::kOk;
    }
    return Status::kDeviceError;
  }

  hipEvent_t raw() const { return event_; }
  void mark_recorded() { recorded_ = true; }

 private:
  hipEvent_t event_ = nullptr;
  DeviceId dev_;
  bool recorded_ = false;
};

inline void clear_sticky_error() { (void)hipGetLastError(); }

}  // namespace

Status RocmBackend::create(int device_index,
                           std::shared_ptr<DeviceBackend>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  int count = 0;
  if (hipGetDeviceCount(&count) != hipSuccess || count <= 0) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  if (device_index < 0 || device_index >= count)
    return Status::kInvalidArgument;
  if (hipSetDevice(device_index) != hipSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  *out = std::make_shared<RocmBackend>(device_index);
  return Status::kOk;
}

DeviceCaps RocmBackend::caps() const {
  DeviceCaps c;
  c.supports_stream = true;
  c.supports_graph_capture = false;
  /* Device memory registers for RDMA on Hygon Z100L at every size measured up
   * to 512 MiB (2026-09-18, rocep227s0f0). */
  c.supports_peer_registration = true;
#if defined(__UCCL_DTK__) || defined(HUX_HIP_NO_DMABUF)
  c.supports_dmabuf_export = false;
#else
  c.supports_dmabuf_export = false; /* Not verified on this stack yet. */
#endif
  c.max_registration_bytes = 0;
  c.max_total_registration_bytes = 0;
  c.supports_ipc = ipc_supported();
  return c;
}

Status RocmBackend::probe_pointer(void const* ptr, DeviceId* dev,
                                  MemoryKind* mem) const {
  if (ptr == nullptr || dev == nullptr || mem == nullptr)
    return Status::kInvalidArgument;

  hipPointerAttribute_t attr;
  std::memset(&attr, 0, sizeof(attr));
  hipError_t e = hipPointerGetAttributes(&attr, ptr);
  if (e != hipSuccess) {
    /* Plain pageable memory the runtime does not track. */
    clear_sticky_error();
    dev->kind = DeviceKind::kHost;
    dev->index = 0;
    *mem = MemoryKind::kHostPageable;
    return Status::kOk;
  }

#if HIP_VERSION_MAJOR >= 6
  hipMemoryType t = attr.type;
#else
  hipMemoryType t = attr.memoryType;
#endif
  if (t == hipMemoryTypeDevice) {
    dev->kind = DeviceKind::kRocm;
    dev->index = attr.device;
    *mem = MemoryKind::kDevice;
  } else if (t == hipMemoryTypeHost) {
    dev->kind = DeviceKind::kHost;
    dev->index = 0;
    *mem = MemoryKind::kHostPinned;
  } else {
    dev->kind = DeviceKind::kHost;
    dev->index = 0;
    *mem = MemoryKind::kHostPageable;
  }
  return Status::kOk;
}

Status RocmBackend::import_stream(void* native_stream, DeviceStreamPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  DeviceId d;
  d.kind = DeviceKind::kRocm;
  d.index = device_index_;
  *out =
      std::make_shared<RocmStream>(static_cast<hipStream_t>(native_stream), d);
  return Status::kOk;
}

Status RocmBackend::record_event(DeviceStream* stream, DeviceEventPtr* out) {
  if (stream == nullptr || out == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<RocmStream*>(stream);
  if (s->device().kind != DeviceKind::kRocm) return Status::kInvalidArgument;

  hipEvent_t ev = nullptr;
  if (hipEventCreateWithFlags(&ev, hipEventDisableTiming) != hipSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  if (hipEventRecord(ev, s->raw()) != hipSuccess) {
    (void)hipEventDestroy(ev);
    clear_sticky_error();
    return Status::kDeviceError;
  }
  auto e = std::make_shared<RocmEvent>(ev, s->device());
  e->mark_recorded();
  *out = e;
  return Status::kOk;
}

Status RocmBackend::stream_wait_event(DeviceStream* stream, DeviceEvent* ev) {
  if (stream == nullptr || ev == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<RocmStream*>(stream);
  auto* e = static_cast<RocmEvent*>(ev);
  if (!e->recorded()) return Status::kInvalidArgument;
  if (hipStreamWaitEvent(s->raw(), e->raw(), 0) != hipSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status RocmBackend::make_visible(DeviceStream* stream, void* addr,
                                 uint64_t bytes) {
  (void)addr;
  (void)bytes;
  if (stream == nullptr) return Status::kInvalidArgument;
  /* Same reasoning as the CUDA backend: called after the transport observed
   * completion, so work queued afterwards sees the bytes. Installing a
   * dependency for a transfer still in flight is DEV-01. */
  return Status::kOk;
}

/* Hygon's DTK 23.10 does not answer the unified-addressing query -- it returns
 * an error and leaves the value untouched -- and yet exports handles perfectly
 * well. Believing the query would report a card with no IPC support when it
 * has it, so where the query cannot answer, the driver is asked directly with
 * a 4 KiB allocation. Done once. */
bool RocmBackend::ipc_supported() const {
  std::call_once(ipc_probe_, [this] {
    int value = 0;
    if (hipDeviceGetAttribute(&value, hipDeviceAttributeUnifiedAddressing,
                              device_index_) == hipSuccess) {
      ipc_supported_ = value != 0;
      return;
    }
    clear_sticky_error();
    void* probe = nullptr;
    if (hipMalloc(&probe, 4096) != hipSuccess) {
      clear_sticky_error();
      ipc_supported_ = false;
      return;
    }
    hipIpcMemHandle_t handle;
    ipc_supported_ = hipIpcGetMemHandle(&handle, probe) == hipSuccess;
    if (!ipc_supported_) clear_sticky_error();
    hipFree(probe);
  });
  return ipc_supported_;
}

Status RocmBackend::copy(void* dst, void const* src, uint64_t bytes) {
  if (dst == nullptr || src == nullptr) return Status::kInvalidArgument;
  if (bytes == 0) return Status::kOk;
  /* Default direction, not an explicit one: either side may be a mapping of
   * another process's allocation, and the runtime knows where each address
   * lives while the caller does not. */
  if (hipMemcpy(dst, src, static_cast<size_t>(bytes), hipMemcpyDefault) !=
      hipSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  /* See the CUDA backend: the copy call alone does not mean the bytes have
   * landed, and everything above this treats the return as if it did. */
  return settle();
}

Status RocmBackend::copy_nowait(void* dst, void const* src, uint64_t bytes) {
  if (dst == nullptr || src == nullptr) return Status::kInvalidArgument;
  if (bytes == 0) return Status::kOk;
  if (hipMemcpy(dst, src, static_cast<size_t>(bytes), hipMemcpyDefault) !=
      hipSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status RocmBackend::settle() {
  if (hipStreamSynchronize(nullptr) != hipSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status RocmBackend::export_ipc(void* addr, uint64_t length, IpcHandle* out) {
  if (addr == nullptr || length == 0 || out == nullptr)
    return Status::kInvalidArgument;

  /* What gets exported is the allocation, so the allocation this address sits
   * in has to be found first. Host memory fails here, which is the honest
   * answer: it cannot be named to another process this way. */
  void* base = nullptr;
  size_t allocated = 0;
  if (hipMemGetAddressRange(
          reinterpret_cast<hipDeviceptr_t*>(&base), &allocated,
          reinterpret_cast<hipDeviceptr_t>(addr)) != hipSuccess) {
    clear_sticky_error();
    return Status::kUnsupported;
  }

  uint64_t const offset =
      reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(base);
  if (offset > allocated || length > allocated - offset)
    return Status::kInvalidArgument;

  hipIpcMemHandle_t handle;
  if (hipIpcGetMemHandle(&handle, base) != hipSuccess) {
    clear_sticky_error();
    return Status::kUnsupported;
  }

  auto const* raw = reinterpret_cast<uint8_t const*>(&handle);
  out->bytes.assign(raw, raw + sizeof(handle));
  out->offset = offset;
  out->allocation_bytes = allocated;
  return Status::kOk;
}

Status RocmBackend::import_ipc(IpcHandle const& handle, void** out) {
  if (out == nullptr || handle.bytes.size() != sizeof(hipIpcMemHandle_t))
    return Status::kInvalidArgument;
  if (handle.offset > handle.allocation_bytes) return Status::kInvalidArgument;

  std::string const key(reinterpret_cast<char const*>(handle.bytes.data()),
                        handle.bytes.size());
  std::lock_guard<std::mutex> g(ipc_mu_);

  auto it = imports_by_handle_.find(key);
  if (it != imports_by_handle_.end()) {
    ++it->second.refs;
    *out = static_cast<char*>(it->second.base) + handle.offset;
    return Status::kOk;
  }

  hipIpcMemHandle_t native;
  std::memcpy(&native, handle.bytes.data(), sizeof(native));
  void* base = nullptr;
  if (hipIpcOpenMemHandle(&base, native, hipIpcMemLazyEnablePeerAccess) !=
      hipSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }

  Import rec;
  rec.base = base;
  rec.bytes = handle.allocation_bytes;
  rec.refs = 1;
  imports_by_handle_[key] = rec;
  imports_by_base_[reinterpret_cast<uintptr_t>(base)] = key;
  *out = static_cast<char*>(base) + handle.offset;
  return Status::kOk;
}

Status RocmBackend::close_ipc(void* mapped) {
  if (mapped == nullptr) return Status::kInvalidArgument;
  auto const addr = reinterpret_cast<uintptr_t>(mapped);

  std::lock_guard<std::mutex> g(ipc_mu_);
  /* The greatest base at or below the address, then a range check: an address
   * from an allocation this process never mapped is refused rather than
   * closing whichever mapping happens to sit below it. */
  auto it = imports_by_base_.upper_bound(addr);
  if (it == imports_by_base_.begin()) return Status::kNotFound;
  --it;

  auto rec = imports_by_handle_.find(it->second);
  if (rec == imports_by_handle_.end()) return Status::kNotFound;
  /* Half-open: base + bytes is the first address outside the mapping, and
   * the next mapping can start exactly there. */
  if (addr - it->first >= rec->second.bytes) return Status::kNotFound;

  if (--rec->second.refs > 0) return Status::kOk;

  void* base = rec->second.base;
  imports_by_base_.erase(it);
  imports_by_handle_.erase(rec);
  if (hipIpcCloseMemHandle(base) != hipSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

}  // namespace hux
