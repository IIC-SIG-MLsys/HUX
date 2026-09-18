/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Allocation note, because the obvious call is the wrong one: cnMallocPeerAble
 * reads as the right choice for peer access, but on Neuware v6 the memory it
 * returns is rejected by ibv_reg_mr with EINVAL at every size, 4 KiB included.
 * Plain cnrtMalloc registers and the NIC reads it back correctly. Measured
 * 2026-09-18 on MLU370-X8 against rocep10s0f0. Anything allocating device
 * memory for transfer here must use the plain call. */
#include "device/neuware_backend.h"

#include <cnrt.h>

#include <cstring>

namespace hux {
namespace {

class NeuwareStream : public DeviceStream {
 public:
  NeuwareStream(cnrtQueue_t q, DeviceId d) : queue_(q), dev_(d) {}
  DeviceId device() const override { return dev_; }
  void* native_handle() const override { return queue_; }
  cnrtQueue_t raw() const { return queue_; }

 private:
  cnrtQueue_t queue_;
  DeviceId dev_;
};

class NeuwareEvent : public DeviceEvent {
 public:
  NeuwareEvent(cnrtNotifier_t n, DeviceId d) : notifier_(n), dev_(d) {}
  ~NeuwareEvent() override {
    if (notifier_ != nullptr) (void)cnrtNotifierDestroy(notifier_);
  }

  DeviceId device() const override { return dev_; }
  bool recorded() const override { return recorded_; }
  void* native_handle() const override { return notifier_; }

  Status query(bool* complete) override {
    if (complete == nullptr) return Status::kInvalidArgument;
    if (!recorded_) {
      *complete = false;
      return Status::kInvalidArgument;
    }
    cnrtRet_t r = cnrtQueryNotifier(notifier_);
    if (r == cnrtSuccess) {
      *complete = true;
      return Status::kOk;
    }
    if (r == cnrtErrorNotReady) {
      *complete = false;
      return Status::kOk;
    }
    return Status::kDeviceError;
  }

  cnrtNotifier_t raw() const { return notifier_; }
  void mark_recorded() { recorded_ = true; }

 private:
  cnrtNotifier_t notifier_ = nullptr;
  DeviceId dev_;
  bool recorded_ = false;
};

}  // namespace

Status NeuwareBackend::create(int device_index,
                              std::shared_ptr<DeviceBackend>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  unsigned int count = 0;
  if (cnrtGetDeviceCount(&count) != cnrtSuccess || count == 0)
    return Status::kDeviceError;
  if (device_index < 0 || static_cast<unsigned>(device_index) >= count)
    return Status::kInvalidArgument;
  if (cnrtSetDevice(device_index) != cnrtSuccess) return Status::kDeviceError;
  *out = std::make_shared<NeuwareBackend>(device_index);
  return Status::kOk;
}

DeviceCaps NeuwareBackend::caps() const {
  DeviceCaps c;
  c.supports_stream = true;
  c.supports_graph_capture = false;
  c.supports_peer_registration = true;
  c.supports_dmabuf_export = false;

  /* Registration is bounded per process, not per call: a single 256 MiB region
   * registers, 320 MiB does not, and four separate 64 MiB regions already
   * exhaust the same quota. Reporting only a per-call limit would let a caller
   * register several regions successfully and then fail on the next one for no
   * visible reason. Measured 2026-09-18 on MLU370-X8. */
  c.max_registration_bytes = 256ull << 20;
  c.max_total_registration_bytes = 256ull << 20;
  return c;
}

Status NeuwareBackend::probe_pointer(void const* ptr, DeviceId* dev,
                                     MemoryKind* mem) const {
  if (ptr == nullptr || dev == nullptr || mem == nullptr)
    return Status::kInvalidArgument;

  cnrtPointerAttributes_t attr;
  std::memset(&attr, 0, sizeof(attr));
  if (cnrtPointerGetAttributes(&attr, ptr) != cnrtSuccess) {
    dev->kind = DeviceKind::kHost;
    dev->index = 0;
    *mem = MemoryKind::kHostPageable;
    return Status::kOk;
  }

  switch (attr.type) {
    case cnrtMemTypeDevice:
      dev->kind = DeviceKind::kCambricon;
      dev->index = attr.device;
      *mem = MemoryKind::kDevice;
      return Status::kOk;
    case cnrtMemTypeHost:
      dev->kind = DeviceKind::kHost;
      dev->index = 0;
      *mem = MemoryKind::kHostPinned;
      return Status::kOk;
    case cnrtMemTypeUnregistered:
    default:
      dev->kind = DeviceKind::kHost;
      dev->index = 0;
      *mem = MemoryKind::kHostPageable;
      return Status::kOk;
  }
}

Status NeuwareBackend::import_stream(void* native_stream,
                                     DeviceStreamPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  DeviceId d;
  d.kind = DeviceKind::kCambricon;
  d.index = device_index_;
  *out = std::make_shared<NeuwareStream>(
      static_cast<cnrtQueue_t>(native_stream), d);
  return Status::kOk;
}

Status NeuwareBackend::record_event(DeviceStream* stream,
                                    DeviceEventPtr* out) {
  if (stream == nullptr || out == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<NeuwareStream*>(stream);
  if (s->device().kind != DeviceKind::kCambricon)
    return Status::kInvalidArgument;

  cnrtNotifier_t n = nullptr;
  if (cnrtNotifierCreate(&n) != cnrtSuccess) return Status::kDeviceError;
  if (cnrtPlaceNotifier(n, s->raw()) != cnrtSuccess) {
    (void)cnrtNotifierDestroy(n);
    return Status::kDeviceError;
  }
  auto e = std::make_shared<NeuwareEvent>(n, s->device());
  e->mark_recorded();
  *out = e;
  return Status::kOk;
}

Status NeuwareBackend::stream_wait_event(DeviceStream* stream,
                                         DeviceEvent* ev) {
  if (stream == nullptr || ev == nullptr) return Status::kInvalidArgument;
  auto* s = static_cast<NeuwareStream*>(stream);
  auto* e = static_cast<NeuwareEvent*>(ev);
  if (!e->recorded()) return Status::kInvalidArgument;
  if (cnrtQueueWaitNotifier(e->raw(), s->raw(), 0) != cnrtSuccess)
    return Status::kDeviceError;
  return Status::kOk;
}

Status NeuwareBackend::make_visible(DeviceStream* stream, void* addr,
                                    uint64_t bytes) {
  (void)addr;
  (void)bytes;
  if (stream == nullptr) return Status::kInvalidArgument;
  return Status::kOk;
}

}  // namespace hux
