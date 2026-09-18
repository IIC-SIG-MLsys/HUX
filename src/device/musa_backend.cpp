/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "device/musa_backend.h"

#include <musa_runtime.h>

#include <cstring>

namespace hux {
namespace {

class MusaStream : public DeviceStream {
 public:
  MusaStream(musaStream_t s, DeviceId d) : stream_(s), dev_(d) {}
  DeviceId device() const override { return dev_; }
  void* native_handle() const override { return stream_; }
  musaStream_t raw() const { return stream_; }

 private:
  musaStream_t stream_;
  DeviceId dev_;
};

class MusaEvent : public DeviceEvent {
 public:
  MusaEvent(musaEvent_t e, DeviceId d) : event_(e), dev_(d) {}
  ~MusaEvent() override {
    if (event_ != nullptr) (void)musaEventDestroy(event_);
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
    musaError_t e = musaEventQuery(event_);
    if (e == musaSuccess) {
      *complete = true;
      return Status::kOk;
    }
    if (e == musaErrorNotReady) {
      *complete = false;
      return Status::kOk;
    }
    return Status::kDeviceError;
  }

  musaEvent_t raw() const { return event_; }
  void mark_recorded() { recorded_ = true; }

 private:
  musaEvent_t event_ = nullptr;
  DeviceId dev_;
  bool recorded_ = false;
};

inline void clear_sticky_error() { (void)musaGetLastError(); }

}  // namespace

Status MusaBackend::create(int device_index, std::shared_ptr<DeviceBackend>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  int count = 0;
  if (musaGetDeviceCount(&count) != musaSuccess || count <= 0) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  if (device_index < 0 || device_index >= count) return Status::kInvalidArgument;
  if (musaSetDevice(device_index) != musaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  *out = std::make_shared<MusaBackend>(device_index);
  return Status::kOk;
}

DeviceCaps MusaBackend::caps() const {
  DeviceCaps c;
  c.supports_stream = true;
  c.supports_graph_capture = false;

  /* The one device so far that cannot register its memory for RDMA at all:
   * ibv_reg_mr on a musaMalloc pointer returns EFAULT at every size, 4 KiB
   * included, while pinned host memory registers at every size up to 512 MiB.
   * Measured 2026-09-18 on S3000 with MUSA 3.1.0 against mlx5_0.
   *
   * Transfers therefore stage through pinned host memory, and no amount of
   * tuning makes this device transfer in place. Reporting true here would make
   * the engine promise zero-copy and then fail at registration. */
  c.supports_peer_registration = false;
  c.supports_dmabuf_export = false;
  c.max_registration_bytes = 0;
  c.max_total_registration_bytes = 0;
  return c;
}

Status MusaBackend::probe_pointer(void const* ptr, DeviceId* dev,
                                  MemoryKind* mem) const {
  if (ptr == nullptr || dev == nullptr || mem == nullptr)
    return Status::kInvalidArgument;

  musaPointerAttributes attr;
  std::memset(&attr, 0, sizeof(attr));
  if (musaPointerGetAttributes(&attr, ptr) != musaSuccess) {
    clear_sticky_error();
    dev->kind = DeviceKind::kHost;
    dev->index = 0;
    *mem = MemoryKind::kHostPageable;
    return Status::kOk;
  }

  switch (attr.type) {
    case musaMemoryTypeDevice:
    case musaMemoryTypeManaged:
      dev->kind = DeviceKind::kMoore;
      dev->index = attr.device;
      *mem = MemoryKind::kDevice;
      return Status::kOk;
    case musaMemoryTypeHost:
      dev->kind = DeviceKind::kHost;
      dev->index = 0;
      *mem = MemoryKind::kHostPinned;
      return Status::kOk;
    default:
      dev->kind = DeviceKind::kHost;
      dev->index = 0;
      *mem = MemoryKind::kHostPageable;
      return Status::kOk;
  }
}

Status MusaBackend::import_stream(void* native_stream, DeviceStreamPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  DeviceId d;
  d.kind = DeviceKind::kMoore;
  d.index = device_index_;
  *out = std::make_shared<MusaStream>(static_cast<musaStream_t>(native_stream), d);
  return Status::kOk;
}

Status MusaBackend::record_event(DeviceStream* stream, DeviceEventPtr* out) {
  if (stream == nullptr || out == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<MusaStream*>(stream);
  if (s->device().kind != DeviceKind::kMoore) return Status::kInvalidArgument;

  musaEvent_t ev = nullptr;
  if (musaEventCreateWithFlags(&ev, musaEventDisableTiming) != musaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  if (musaEventRecord(ev, s->raw()) != musaSuccess) {
    (void)musaEventDestroy(ev);
    clear_sticky_error();
    return Status::kDeviceError;
  }
  auto e = std::make_shared<MusaEvent>(ev, s->device());
  e->mark_recorded();
  *out = e;
  return Status::kOk;
}

Status MusaBackend::stream_wait_event(DeviceStream* stream, DeviceEvent* ev) {
  if (stream == nullptr || ev == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<MusaStream*>(stream);
  auto* e = static_cast<MusaEvent*>(ev);
  if (!e->recorded()) return Status::kInvalidArgument;
  if (musaStreamWaitEvent(s->raw(), e->raw(), 0) != musaSuccess) {
    clear_sticky_error();
    return Status::kDeviceError;
  }
  return Status::kOk;
}

Status MusaBackend::make_visible(DeviceStream* stream, void* addr,
                                 uint64_t bytes) {
  (void)addr;
  (void)bytes;
  if (stream == nullptr) return Status::kInvalidArgument;
  /* Data lands in pinned host memory that is mapped into the device address
   * space, so there is no device-side visibility step once the transport has
   * reported completion. */
  return Status::kOk;
}

}  // namespace hux
