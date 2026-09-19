/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_TESTS_FAKE_IPC_BACKEND_H
#define HUX_TESTS_FAKE_IPC_BACKEND_H

#include <cstring>

#include "hux/device.h"

namespace hux {
namespace testing {

/* Stands in for a vendor that can name an allocation. Both ends live in this
 * process, so a handle is just the address, and mapping returns it unchanged
 * -- which is exactly what a real mapping means to the code above it. */
class FakeIpcBackend : public DeviceBackend {
 public:
  DeviceKind kind() const override { return DeviceKind::kHost; }
  DeviceCaps caps() const override {
    DeviceCaps c;
    c.supports_ipc = true;
    return c;
  }
  Status probe_pointer(void const*, DeviceId* dev,
                       MemoryKind* mem) const override {
    if (dev != nullptr) *dev = DeviceId{};
    if (mem != nullptr) *mem = MemoryKind::kHostPageable;
    return Status::kOk;
  }
  Status import_stream(void*, DeviceStreamPtr*) override {
    return Status::kUnsupported;
  }
  Status record_event(DeviceStream*, DeviceEventPtr*) override {
    return Status::kUnsupported;
  }
  Status stream_wait_event(DeviceStream*, DeviceEvent*) override {
    return Status::kUnsupported;
  }
  Status make_visible(DeviceStream*, void*, uint64_t) override {
    return Status::kOk;
  }
  Status copy(void* dst, void const* src, uint64_t bytes) override {
    std::memcpy(dst, src, static_cast<size_t>(bytes));
    return Status::kOk;
  }
  Status export_ipc(void* addr, uint64_t length, IpcHandle* out) override {
    auto const v = reinterpret_cast<uintptr_t>(addr);
    out->bytes.assign(reinterpret_cast<uint8_t const*>(&v),
                      reinterpret_cast<uint8_t const*>(&v) + sizeof(v));
    out->offset = 0;
    out->allocation_bytes = length;
    return Status::kOk;
  }
  Status import_ipc(IpcHandle const& handle, void** out) override {
    if (handle.bytes.size() != sizeof(uintptr_t))
      return Status::kInvalidArgument;
    uintptr_t v = 0;
    std::memcpy(&v, handle.bytes.data(), sizeof(v));
    ++opened;
    *out = reinterpret_cast<void*>(v + handle.offset);
    return Status::kOk;
  }
  Status close_ipc(void*) override {
    ++closed;
    return Status::kOk;
  }

  int opened = 0;
  int closed = 0;
};

/* A device that can do everything above except name an allocation to another
 * process -- which is what host memory looks like to every real backend. */
class NoIpcBackend : public FakeIpcBackend {
 public:
  DeviceCaps caps() const override {
    DeviceCaps c;
    c.supports_ipc = false;
    return c;
  }
  Status export_ipc(void*, uint64_t, IpcHandle*) override {
    return Status::kUnsupported;
  }
};

}  // namespace testing
}  // namespace hux
#endif  // HUX_TESTS_FAKE_IPC_BACKEND_H
