/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "device/cuda_backend.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <cstring>

namespace hux {
namespace {

class CudaStream : public DeviceStream {
 public:
  CudaStream(cudaStream_t s, DeviceId d) : stream_(s), dev_(d) {}
  DeviceId device() const override { return dev_; }
  void* native_handle() const override { return stream_; }
  cudaStream_t raw() const { return stream_; }

 private:
  cudaStream_t stream_;
  DeviceId dev_;
};

class CudaEvent : public DeviceEvent {
 public:
  CudaEvent(cudaEvent_t e, DeviceId d) : event_(e), dev_(d) {}
  ~CudaEvent() override {
    if (event_ != nullptr) cudaEventDestroy(event_);
  }

  DeviceId device() const override { return dev_; }
  bool recorded() const override { return recorded_; }
  void* native_handle() const override { return event_; }

  Status query(bool* complete) override {
    if (complete == nullptr) return Status::kInvalidArgument;
    /* An event that was never recorded captures no work. Reporting it as
     * complete would let a caller build a dependency on nothing. */
    if (!recorded_) {
      *complete = false;
      return Status::kInvalidArgument;
    }
    cudaError_t e = cudaEventQuery(event_);
    if (e == cudaSuccess) {
      *complete = true;
      return Status::kOk;
    }
    if (e == cudaErrorNotReady) {
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

/* The runtime keeps a sticky error after a failed call; clearing it here stops
 * an unrelated later call from inheriting the failure. */
inline void clear_sticky_error() { (void)cudaGetLastError(); }

/* Makes a device current for the calls in scope and puts back whatever was
 * current before. The runtime's current device is per thread, and was set
 * only on the thread that created the backend: called from the progress
 * thread, or any caller's, settle() synchronised device 0's stream rather
 * than this device's -- so copy() said bytes had landed that had not -- an
 * IPC mapping went into device 0's context, and an event was created on
 * device 0 for a stream on another. */
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

}  // namespace

Status CudaBackend::create(int device_index,
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
    /* Proves the device can be made current, without leaving it current on
     * the caller's thread: that was a side effect nobody asked for. */
    int prev = -1;
    if (cudaGetDevice(&prev) != cudaSuccess) clear_sticky_error();
    if (cudaSetDevice(device_index) != cudaSuccess) {
      clear_sticky_error();
      return Status::kDeviceError;
    }
    if (prev >= 0 && prev != device_index) cudaSetDevice(prev);
  }
  *out = std::make_shared<CudaBackend>(device_index);
  return Status::kOk;
}

DeviceCaps CudaBackend::caps() const {
  DeviceCaps c;
  c.supports_stream = true;
  c.supports_graph_capture = false; /* Tracked separately; not yet verified. */
  c.supports_peer_registration = true;
  /* DMA-BUF export needs both a recent driver and kernel support, so it is
   * probed rather than assumed. */
  c.supports_dmabuf_export = false;
  int value = 0;
  if (cudaDeviceGetAttribute(&value, cudaDevAttrGPUDirectRDMASupported,
                             device_index_) == cudaSuccess) {
    c.supports_dmabuf_export = value != 0;
  } else {
    clear_sticky_error();
  }
  c.max_registration_bytes = 0; /* No practical cap on NVIDIA. */
  /* Unified addressing is what IPC is built on, so a device without it cannot
   * export an allocation whatever else it supports. */
  value = 0;
  if (cudaDeviceGetAttribute(&value, cudaDevAttrUnifiedAddressing,
                             device_index_) == cudaSuccess) {
    c.supports_ipc = value != 0;
  } else {
    clear_sticky_error();
  }
  return c;
}

Status CudaBackend::probe_pointer(void const* ptr, DeviceId* dev,
                                  MemoryKind* mem) const {
  if (ptr == nullptr || dev == nullptr || mem == nullptr)
    return Status::kInvalidArgument;

  cudaPointerAttributes attr;
  std::memset(&attr, 0, sizeof(attr));
  cudaError_t e = cudaPointerGetAttributes(&attr, ptr);
  if (e != cudaSuccess) {
    /* Unregistered host memory is not an error: it is ordinary pageable
     * memory the runtime simply does not know about. */
    clear_sticky_error();
    dev->kind = DeviceKind::kHost;
    dev->index = 0;
    *mem = MemoryKind::kHostPageable;
    return Status::kOk;
  }

  switch (attr.type) {
    case cudaMemoryTypeDevice:
      dev->kind = DeviceKind::kCuda;
      dev->index = attr.device;
      *mem = MemoryKind::kDevice;
      return Status::kOk;
    case cudaMemoryTypeHost:
      dev->kind = DeviceKind::kHost;
      dev->index = 0;
      *mem = MemoryKind::kHostPinned;
      return Status::kOk;
    case cudaMemoryTypeManaged:
      /* Managed memory migrates between host and device, so it is reported as
       * device memory on the device that currently owns it. */
      dev->kind = DeviceKind::kCuda;
      dev->index = attr.device;
      *mem = MemoryKind::kDevice;
      return Status::kOk;
    default:
      dev->kind = DeviceKind::kHost;
      dev->index = 0;
      *mem = MemoryKind::kHostPageable;
      return Status::kOk;
  }
}

Status CudaBackend::import_stream(void* native_stream, DeviceStreamPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  /* A null handle is the legacy default stream. Accepting it is allowed, but
   * callers are expected to pass their own stream: the default stream
   * serializes against everything else on the device. */
  DeviceId d;
  d.kind = DeviceKind::kCuda;
  d.index = device_index_;
  *out =
      std::make_shared<CudaStream>(static_cast<cudaStream_t>(native_stream), d);
  return Status::kOk;
}

Status CudaBackend::record_event(DeviceStream* stream, DeviceEventPtr* out) {
  if (stream == nullptr || out == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<CudaStream*>(stream);
  if (s->device().kind != DeviceKind::kCuda) return Status::kInvalidArgument;

  /* The event belongs on the stream's device; recording one made on
   * another device into it fails. */
  DeviceGuard guard(s->device().index);
  cudaEvent_t ev = nullptr;
  /* Disabling timing keeps the record and wait path cheap; HUX never reads
   * elapsed time from these events. */
  if (cudaEventCreateWithFlags(&ev, cudaEventDisableTiming) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  if (cudaEventRecord(ev, s->raw()) != cudaSuccess) {
    cudaEventDestroy(ev);
    clear_sticky_error();
    return Status::kDeviceError;
  }
  auto e = std::make_shared<CudaEvent>(ev, s->device());
  e->mark_recorded();
  *out = e;
  return Status::kOk;
}

Status CudaBackend::stream_wait_event(DeviceStream* stream, DeviceEvent* ev) {
  if (stream == nullptr || ev == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<CudaStream*>(stream);
  auto* e = static_cast<CudaEvent*>(ev);
  /* CUDA waits on the work an event already captured, so waiting on an
   * unrecorded event would silently order nothing. */
  if (!e->recorded()) return Status::kInvalidArgument;
  if (cudaStreamWaitEvent(s->raw(), e->raw(), 0) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status CudaBackend::make_visible(DeviceStream* stream, void* addr,
                                 uint64_t bytes) {
  (void)addr;
  (void)bytes;
  if (stream == nullptr) return Status::kInvalidArgument;
  /* Called once the transport has observed completion, i.e. the NIC has
   * acknowledged the write to device memory. Work launched onto a stream
   * afterwards observes those bytes, so no extra device-side step is needed
   * here.
   *
   * This holds only because the caller already saw the completion. Installing
   * a dependency for a transfer that is still in flight needs a GPU-side wait
   * primitive (stream memory operations) and is not implemented yet -- see
   * DEV-01. Until then the engine reaches target_ready only after completion,
   * never before. */
  return Status::kOk;
}

Status CudaBackend::copy(void* dst, void const* src, uint64_t bytes) {
  if (dst == nullptr || src == nullptr) return Status::kInvalidArgument;
  if (bytes == 0) return Status::kOk;
  DeviceGuard guard(device_index_);
  /* cudaMemcpyDefault, not an explicit direction: one side may be a mapping
   * of another process's allocation, and the runtime knows where each address
   * lives while the caller does not. */
  if (cudaMemcpy(dst, src, static_cast<size_t>(bytes), cudaMemcpyDefault) !=
      cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  /* And then wait, because cudaMemcpy on its own does not mean the bytes have
   * landed: device-to-device does no host-side synchronization, and a
   * pageable host source returns once the staging copy is done. Measured:
   * without this, a peer told "the data is ready" reads a buffer that is
   * still partly the previous contents, roughly once in 30000 rounds. */
  return settle();
}

Status CudaBackend::copy_nowait(void* dst, void const* src, uint64_t bytes) {
  if (dst == nullptr || src == nullptr) return Status::kInvalidArgument;
  if (bytes == 0) return Status::kOk;
  DeviceGuard guard(device_index_);
  if (cudaMemcpy(dst, src, static_cast<size_t>(bytes), cudaMemcpyDefault) !=
      cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status CudaBackend::settle() {
  /* The default stream of this device, which is the one the copies went to
   * -- not whichever device the calling thread happens to have current. */
  DeviceGuard guard(device_index_);
  if (cudaStreamSynchronize(nullptr) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status CudaBackend::export_ipc(void* addr, uint64_t length, IpcHandle* out) {
  if (addr == nullptr || length == 0 || out == nullptr)
    return Status::kInvalidArgument;

  /* What gets exported is the allocation, so the allocation this address sits
   * in has to be found first. Host memory and anything not allocated on the
   * device fail here, which is the honest answer: neither can be named to
   * another process this way. */
  CUdeviceptr base = 0;
  size_t allocated = 0;
  if (cuMemGetAddressRange(&base, &allocated,
                           reinterpret_cast<CUdeviceptr>(addr)) != CUDA_SUCCESS)
    return Status::kUnsupported;

  uint64_t const offset =
      reinterpret_cast<uintptr_t>(addr) - static_cast<uintptr_t>(base);
  /* Refused rather than clamped: an importer told the span fits would map the
   * allocation and read past its end. */
  if (offset > allocated || length > allocated - offset)
    return Status::kInvalidArgument;

  cudaIpcMemHandle_t handle;
  DeviceGuard guard(device_index_);
  if (cudaIpcGetMemHandle(&handle, reinterpret_cast<void*>(base)) !=
      cudaSuccess) {
    clear_sticky_error();
    return Status::kUnsupported;
  }

  auto const* raw = reinterpret_cast<uint8_t const*>(&handle);
  out->bytes.assign(raw, raw + sizeof(handle));
  out->offset = offset;
  out->allocation_bytes = allocated;
  return Status::kOk;
}

Status CudaBackend::import_ipc(IpcHandle const& handle, void** out) {
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
  /* Mapped into this device's context, not the calling thread's. */
  DeviceGuard guard(device_index_);
  /* Lazy peer access: enabling it eagerly would fail on a pair of devices
   * that cannot reach each other, for an allocation the caller may only ever
   * read from its own side. */
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

Status CudaBackend::close_ipc(void* mapped) {
  if (mapped == nullptr) return Status::kInvalidArgument;
  auto const addr = reinterpret_cast<uintptr_t>(mapped);

  std::lock_guard<std::mutex> g(ipc_mu_);
  /* The greatest base at or below the address, then a range check: an address
   * from an allocation this process never mapped must be refused rather than
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
  DeviceGuard guard(device_index_);
  if (cudaIpcCloseMemHandle(base) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

}  // namespace hux
