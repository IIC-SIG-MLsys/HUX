/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "device/cuda_backend.h"

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
  if (cudaSetDevice(device_index) != cudaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
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

}  // namespace hux
