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
  /* The driver exports handles for device allocations and for pinned host
   * memory, so this is reported rather than probed per call -- export_ipc
   * refuses anything it cannot name. */
  c.supports_ipc = true;
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

Status NeuwareBackend::record_event(DeviceStream* stream, DeviceEventPtr* out) {
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

Status NeuwareBackend::copy(void* dst, void const* src, uint64_t bytes) {
  if (dst == nullptr || src == nullptr) return Status::kInvalidArgument;
  if (bytes == 0) return Status::kOk;
  /* No direction given: either side may be a mapping of another process's
   * allocation, and the runtime resolves where each address lives. */
  if (cnrtMemcpy(dst, const_cast<void*>(src), static_cast<size_t>(bytes),
                 cnrtMemcpyNoDirection) != cnrtSuccess)
    return Status::kDeviceError;
  /* See the CUDA backend: the copy call alone does not mean the bytes have
   * landed, and everything above this treats the return as if it did. */
  return settle();
}

Status NeuwareBackend::copy_nowait(void* dst, void const* src, uint64_t bytes) {
  if (dst == nullptr || src == nullptr) return Status::kInvalidArgument;
  if (bytes == 0) return Status::kOk;
  if (cnrtMemcpy(dst, const_cast<void*>(src), static_cast<size_t>(bytes),
                 cnrtMemcpyNoDirection) != cnrtSuccess)
    return Status::kDeviceError;
  return Status::kOk;
}

Status NeuwareBackend::settle() {
  return cnrtSyncDevice() == cnrtSuccess ? Status::kOk : Status::kDeviceError;
}

Status NeuwareBackend::export_ipc(void* addr, uint64_t length, IpcHandle* out) {
  if (addr == nullptr || length == 0 || out == nullptr)
    return Status::kInvalidArgument;

  /* There is no call here for finding the allocation an address belongs to,
   * and none is needed: the driver refuses an address that is not one it
   * allocated. Measured on MLU370-X8 -- a handle for base+4096 comes back
   * CN_MEMORY_ERROR_INVALID_ADDRESS. So a region inside a larger allocation
   * is refused rather than exported with an offset this backend cannot
   * compute, which would map the right memory and read the wrong bytes. */
  cnrtIpcMemHandle handle;
  std::memset(&handle, 0, sizeof(handle));
  if (cnrtAcquireMemHandle(&handle, addr) != cnrtSuccess)
    return Status::kUnsupported;

  auto const* raw = reinterpret_cast<uint8_t const*>(&handle);
  out->bytes.assign(raw, raw + sizeof(handle));
  out->offset = 0;
  /* The allocation's real size is not reported by this driver, so what the
   * caller registered is what is claimed. It is used only to bound the
   * lookup that finds a mapping from an address inside it. */
  out->allocation_bytes = length;
  return Status::kOk;
}

Status NeuwareBackend::import_ipc(IpcHandle const& handle, void** out) {
  if (out == nullptr || handle.bytes.size() != sizeof(cnrtIpcMemHandle))
    return Status::kInvalidArgument;
  if (handle.offset != 0) return Status::kInvalidArgument;

  std::string const key(reinterpret_cast<char const*>(handle.bytes.data()),
                        handle.bytes.size());
  std::lock_guard<std::mutex> g(ipc_mu_);

  auto it = imports_by_handle_.find(key);
  if (it != imports_by_handle_.end()) {
    ++it->second.refs;
    *out = it->second.base;
    return Status::kOk;
  }

  cnrtIpcMemHandle native;
  std::memcpy(&native, handle.bytes.data(), sizeof(native));
  void* base = nullptr;
  if (cnrtMapMemHandle(&base, native, 0) != cnrtSuccess)
    return Status::kDeviceError;

  Import rec;
  rec.base = base;
  rec.bytes = handle.allocation_bytes;
  rec.refs = 1;
  imports_by_handle_[key] = rec;
  imports_by_base_[reinterpret_cast<uintptr_t>(base)] = key;
  *out = base;
  return Status::kOk;
}

Status NeuwareBackend::close_ipc(void* mapped) {
  if (mapped == nullptr) return Status::kInvalidArgument;
  auto const addr = reinterpret_cast<uintptr_t>(mapped);

  std::lock_guard<std::mutex> g(ipc_mu_);
  auto it = imports_by_base_.upper_bound(addr);
  if (it == imports_by_base_.begin()) return Status::kNotFound;
  --it;

  auto rec = imports_by_handle_.find(it->second);
  if (rec == imports_by_handle_.end()) return Status::kNotFound;
  if (addr - it->first > rec->second.bytes) return Status::kNotFound;

  if (--rec->second.refs > 0) return Status::kOk;

  void* base = rec->second.base;
  imports_by_base_.erase(it);
  imports_by_handle_.erase(rec);
  if (cnrtUnMapMemHandle(base) != cnrtSuccess) return Status::kDeviceError;
  return Status::kOk;
}

}  // namespace hux
