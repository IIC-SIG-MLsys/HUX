/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Measured on P800 (XPU 5.18.0.0), because each of these would otherwise be
 * assumed from the CUDA runtime this one imitates:
 *
 *   - cudaMemcpyDefault returns cudaErrorInvalidValue. Every copy here
 *     states its direction, worked out from both addresses.
 *   - cudaHostRegister succeeds, and the memory it pinned still reports
 *     cudaMemoryTypeUnregistered, so pinned host memory cannot be told from
 *     pageable. probe_pointer reports host memory as pageable rather than
 *     claiming to know.
 *   - ibv_reg_mr over device memory fails with EFAULT at every size, 4 KiB
 *     included, while host memory registers. There is no GPUDirect here.
 *   - there is no libcuda: cuMemGetAddressRange does not exist, and device
 *     allocations are not in /proc/self/maps, so an allocation's base cannot
 *     be looked up. See export_ipc. */
#include "device/kunlun_backend.h"

#include <cuda_runtime.h>

#include <cstring>

namespace hux {
namespace {

class KunlunStream : public DeviceStream {
 public:
  KunlunStream(cudaStream_t s, DeviceId d) : stream_(s), dev_(d) {}
  DeviceId device() const override { return dev_; }
  void* native_handle() const override { return stream_; }
  cudaStream_t raw() const { return stream_; }

 private:
  cudaStream_t stream_;
  DeviceId dev_;
};

class KunlunEvent : public DeviceEvent {
 public:
  KunlunEvent(cudaEvent_t e, DeviceId d) : event_(e), dev_(d) {}
  ~KunlunEvent() override {
    if (event_ != nullptr) (void)cudaEventDestroy(event_);
  }

  DeviceId device() const override { return dev_; }
  bool recorded() const override { return recorded_; }
  void* native_handle() const override { return event_; }

  Status query(bool* complete) override {
    if (complete == nullptr) return Status::kInvalidArgument;
    if (!recorded_) {
      /* Never recorded: it captures no work, so it can never be complete.
       * Reporting it as satisfied would release the NIC against data that
       * does not exist yet. */
      *complete = false;
      return Status::kInvalidArgument;
    }
    cudaError_t r = cudaEventQuery(event_);
    if (r == cudaSuccess) {
      *complete = true;
      return Status::kOk;
    }
    if (r == cudaErrorNotReady) {
      *complete = false;
      return Status::kOk;
    }
    return Status::kDeviceError;
  }

  cudaEvent_t raw() const { return event_; }
  void mark_recorded() { recorded_ = true; }

 private:
  cudaEvent_t event_ = nullptr;
  DeviceId dev_;
  bool recorded_ = false;
};

/* The runtime keeps a sticky error after a failed call; clearing it stops an
 * unrelated later call from inheriting the failure. */
inline void clear_sticky_error() { (void)cudaGetLastError(); }

/* Makes a device current for the calls in scope and puts back what was
 * current before. The current device is per thread and this backend is
 * called from whichever thread drives progress, not only the one that
 * created it. */
class DeviceGuard {
 public:
  explicit DeviceGuard(int want) {
    if (cudaGetDevice(&prev_) != cudaSuccess) {
      clear_sticky_error();
      prev_ = -1;
    }
    if (prev_ == want) return;
    if (cudaSetDevice(want) == cudaSuccess)
      set_ = true;
    else
      clear_sticky_error();
  }
  ~DeviceGuard() {
    if (set_ && prev_ >= 0) cudaSetDevice(prev_);
  }
  DeviceGuard(DeviceGuard const&) = delete;
  DeviceGuard& operator=(DeviceGuard const&) = delete;

 private:
  int prev_ = -1;
  bool set_ = false;
};

bool on_device(void const* p) {
  cudaPointerAttributes a{};
  cudaError_t const e = cudaPointerGetAttributes(&a, p);
  if (e != cudaSuccess) {
    clear_sticky_error();
    return false;
  }
  return a.type == cudaMemoryTypeDevice;
}

/* Which direction to copy in. cudaMemcpyDefault is refused here, so the
 * direction is decided from the two addresses. A mapping of another
 * process's allocation reports as device memory, which is what it is. */
cudaMemcpyKind direction(void const* dst, void const* src) {
  bool const d = on_device(dst);
  bool const s = on_device(src);
  if (d && s) return cudaMemcpyDeviceToDevice;
  if (d) return cudaMemcpyHostToDevice;
  if (s) return cudaMemcpyDeviceToHost;
  return cudaMemcpyHostToHost;
}

/* Whether an address is the start of the allocation it belongs to.
 *
 * A handle names the whole allocation, and an interior address yields the
 * same handle as its base -- so an interior address cannot be exported as if
 * it were a base, which would map the right allocation and hand back the
 * wrong bytes. Without a way to look up the base, this is what can be
 * established: the byte before a base belongs to another allocation or to
 * none. Checked against adjacent allocations, where the byte before one base
 * is the last byte of another. */
bool is_allocation_base(void* p) {
  cudaIpcMemHandle_t here;
  if (cudaIpcGetMemHandle(&here, p) != cudaSuccess) {
    clear_sticky_error();
    return false;
  }
  cudaIpcMemHandle_t before;
  cudaError_t const e = cudaIpcGetMemHandle(&before, static_cast<char*>(p) - 1);
  clear_sticky_error();
  if (e != cudaSuccess) return true;
  return std::memcmp(&here, &before, sizeof(here)) != 0;
}

}  // namespace

Status KunlunBackend::create(int device_index,
                             std::shared_ptr<DeviceBackend>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  if (device_index < 0 || device_index >= count)
    return Status::kInvalidArgument;
  {
    /* Proves the device can be made current without leaving it current on
     * the caller's thread. */
    int prev = -1;
    if (cudaGetDevice(&prev) != cudaSuccess) clear_sticky_error();
    if (cudaSetDevice(device_index) != cudaSuccess) {
      clear_sticky_error();
      return Status::kDeviceError;
    }
    if (prev >= 0 && prev != device_index) cudaSetDevice(prev);
  }
  *out = std::make_shared<KunlunBackend>(device_index);
  return Status::kOk;
}

DeviceCaps KunlunBackend::caps() const {
  DeviceCaps c;
  c.supports_stream = true;
  c.supports_graph_capture = false;
  /* The NIC cannot register device memory here: ibv_reg_mr fails with EFAULT
   * at 4 KiB as at 4 MiB, while host memory registers. Transfers off this
   * host stage through host memory. */
  c.supports_peer_registration = false;
  c.supports_dmabuf_export = false;
  /* Measured across two processes: the second maps the allocation, reads
   * what the first wrote, and writes back into it. */
  c.supports_ipc = true;
  c.max_registration_bytes = 0;
  c.max_total_registration_bytes = 0;
  return c;
}

Status KunlunBackend::probe_pointer(void const* ptr, DeviceId* dev,
                                    MemoryKind* mem) const {
  if (ptr == nullptr || dev == nullptr || mem == nullptr)
    return Status::kInvalidArgument;

  cudaPointerAttributes attr{};
  if (cudaPointerGetAttributes(&attr, ptr) != cudaSuccess) {
    clear_sticky_error();
    dev->kind = DeviceKind::kHost;
    dev->index = 0;
    *mem = MemoryKind::kHostPageable;
    return Status::kOk;
  }
  if (attr.type == cudaMemoryTypeDevice) {
    dev->kind = DeviceKind::kKunlun;
    dev->index = attr.device;
    *mem = MemoryKind::kDevice;
    return Status::kOk;
  }
  /* Everything else is host memory. Pinned is not reported as such here --
   * memory this runtime has pinned still reads back as unregistered -- so it
   * is called pageable rather than guessed at. */
  dev->kind = DeviceKind::kHost;
  dev->index = 0;
  *mem = MemoryKind::kHostPageable;
  return Status::kOk;
}

Status KunlunBackend::import_stream(void* native_stream, DeviceStreamPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  DeviceId d;
  d.kind = DeviceKind::kKunlun;
  d.index = device_index_;
  *out = std::make_shared<KunlunStream>(
      static_cast<cudaStream_t>(native_stream), d);
  return Status::kOk;
}

Status KunlunBackend::record_event(DeviceStream* stream, DeviceEventPtr* out) {
  if (stream == nullptr || out == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<KunlunStream*>(stream);
  if (s->device().kind != DeviceKind::kKunlun) return Status::kInvalidArgument;

  DeviceGuard guard(s->device().index);
  cudaEvent_t ev = nullptr;
  if (cudaEventCreateWithFlags(&ev, cudaEventDisableTiming) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  if (cudaEventRecord(ev, s->raw()) != cudaSuccess) {
    cudaEventDestroy(ev);
    clear_sticky_error();
    return Status::kDeviceError;
  }
  auto e = std::make_shared<KunlunEvent>(ev, s->device());
  e->mark_recorded();
  *out = e;
  return Status::kOk;
}

Status KunlunBackend::stream_wait_event(DeviceStream* stream, DeviceEvent* ev) {
  if (stream == nullptr || ev == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<KunlunStream*>(stream);
  auto* e = static_cast<KunlunEvent*>(ev);
  /* Waiting on an event that captured no work orders nothing. */
  if (!e->recorded()) return Status::kInvalidArgument;
  if (cudaStreamWaitEvent(s->raw(), e->raw(), 0) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status KunlunBackend::make_visible(DeviceStream* stream, void* addr,
                                   uint64_t bytes) {
  (void)addr;
  (void)bytes;
  if (stream == nullptr) return Status::kInvalidArgument;
  /* Called once the transport has seen its completion, so the bytes are
   * already there for work launched afterwards. Ordering against a transfer
   * still in flight would need a device-side wait, which is not implemented;
   * the engine reaches target_ready only after completion. */
  return Status::kOk;
}

Status KunlunBackend::copy(void* dst, void const* src, uint64_t bytes) {
  if (dst == nullptr || src == nullptr) return Status::kInvalidArgument;
  if (bytes == 0) return Status::kOk;
  DeviceGuard guard(device_index_);
  if (cudaMemcpy(dst, src, static_cast<size_t>(bytes), direction(dst, src)) !=
      cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  /* And then wait: the copy call returning is not the bytes having landed,
   * and everything above this treats it as if it were. */
  return settle();
}

Status KunlunBackend::copy_nowait(void* dst, void const* src, uint64_t bytes) {
  if (dst == nullptr || src == nullptr) return Status::kInvalidArgument;
  if (bytes == 0) return Status::kOk;
  DeviceGuard guard(device_index_);
  if (cudaMemcpy(dst, src, static_cast<size_t>(bytes), direction(dst, src)) !=
      cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status KunlunBackend::settle() {
  DeviceGuard guard(device_index_);
  if (cudaStreamSynchronize(nullptr) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status KunlunBackend::export_ipc(void* addr, uint64_t length, IpcHandle* out) {
  if (addr == nullptr || length == 0 || out == nullptr)
    return Status::kInvalidArgument;

  DeviceGuard guard(device_index_);
  /* A handle names the allocation, and the importer is given its base. The
   * offset of an interior address within it cannot be computed here: there
   * is no driver API to look the allocation up, and device memory does not
   * appear in /proc/self/maps. An interior address exported as a base would
   * map the right allocation and read the wrong bytes -- silently -- so it
   * is refused instead. */
  if (!is_allocation_base(addr)) return Status::kUnsupported;

  cudaIpcMemHandle_t handle;
  if (cudaIpcGetMemHandle(&handle, addr) != cudaSuccess) {
    clear_sticky_error();
    return Status::kUnsupported;
  }

  auto const* raw = reinterpret_cast<uint8_t const*>(&handle);
  out->bytes.assign(raw, raw + sizeof(handle));
  out->offset = 0;
  /* The allocation's real size is not reported, so what the caller
   * registered is what is claimed. It only bounds the lookup that finds a
   * mapping from an address inside it. */
  out->allocation_bytes = length;
  return Status::kOk;
}

Status KunlunBackend::import_ipc(IpcHandle const& handle, void** out) {
  if (out == nullptr || handle.bytes.size() != sizeof(cudaIpcMemHandle_t))
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

  cudaIpcMemHandle_t native;
  std::memcpy(&native, handle.bytes.data(), sizeof(native));
  void* base = nullptr;
  DeviceGuard guard(device_index_);
  if (cudaIpcOpenMemHandle(&base, native, cudaIpcMemLazyEnablePeerAccess) !=
      cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }

  Import rec;
  rec.base = base;
  rec.bytes = handle.allocation_bytes;
  rec.handle = key;
  rec.refs = 1;
  imports_by_handle_[key] = rec;
  imports_by_base_[reinterpret_cast<uintptr_t>(base)] = key;
  *out = static_cast<char*>(base) + handle.offset;
  return Status::kOk;
}

Status KunlunBackend::close_ipc(void* mapped) {
  if (mapped == nullptr) return Status::kInvalidArgument;
  auto const addr = reinterpret_cast<uintptr_t>(mapped);

  std::lock_guard<std::mutex> g(ipc_mu_);
  /* The greatest base at or below the address, then a range check: an
   * address from an allocation this process never mapped is refused rather
   * than closing whichever mapping happens to sit below it. */
  auto it = imports_by_base_.upper_bound(addr);
  if (it == imports_by_base_.begin()) return Status::kNotFound;
  --it;

  auto rec = imports_by_handle_.find(it->second);
  if (rec == imports_by_handle_.end()) return Status::kNotFound;
  /* Half-open: base + bytes is the first address outside the mapping. */
  if (addr - it->first >= rec->second.bytes) return Status::kNotFound;

  if (--rec->second.refs > 0) return Status::kOk;

  void* base = rec->second.base;
  imports_by_base_.erase(it);
  imports_by_handle_.erase(rec);
  DeviceGuard guard(device_index_);
  if (cudaIpcCloseMemHandle(base) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

}  // namespace hux
