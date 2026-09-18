/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_DEVICE_H
#define HUX_DEVICE_H

#include <cstdint>
#include <memory>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

/* Handle to an application execution queue. The vendor handle (cudaStream_t,
 * hipStream_t, cnrtQueue_t) stays inside DeviceBackend. The caller keeps the
 * stream alive until all related GPU waits and memory accesses are done. */
class DeviceStream {
 public:
  virtual ~DeviceStream() = default;
  virtual DeviceId device() const = 0;
  virtual void* native_handle() const = 0;
};

/* Vendor semantics differ: CUDA waits only on work already captured by the
 * event, while HIP may report success for an event that was never recorded.
 * An unrecorded event is therefore not a generic "wait for the future" signal. */
class DeviceEvent {
 public:
  virtual ~DeviceEvent() = default;
  virtual DeviceId device() const = 0;
  virtual bool recorded() const = 0;
  virtual Status query(bool* complete) = 0;
  virtual void* native_handle() const = 0;
};

using DeviceStreamPtr = std::shared_ptr<DeviceStream>;
using DeviceEventPtr = std::shared_ptr<DeviceEvent>;

/* Capabilities are reported as they are. A backend never fakes a promised
 * capability with a silent sync or an extra copy. */
struct DeviceCaps {
  bool supports_stream = false;
  bool supports_graph_capture = false;  /* Tracked separately from streams. */
  bool supports_peer_registration = false;
  bool supports_dmabuf_export = false;  /* False on Hygon DTK. */

  /* Largest single registration, 0 if unbounded. Cambricon MLU tops out near
   * 32 MiB and varies with fragmentation; callers must see that limit here
   * rather than discover it at run time. */
  uint64_t max_registration_bytes = 0;
};

/* One implementation per vendor, each independently buildable and testable. */
class DeviceBackend {
 public:
  virtual ~DeviceBackend() = default;

  virtual DeviceKind kind() const = 0;
  virtual DeviceCaps caps() const = 0;

  /* Which device and memory kind a pointer belongs to. Never trust the caller's
   * claim for this. */
  virtual Status probe_pointer(void const* ptr, DeviceId* dev,
                               MemoryKind* mem) const = 0;

  /* Imports an application stream without taking ownership. */
  virtual Status import_stream(void* native_stream, DeviceStreamPtr* out) = 0;
  virtual Status record_event(DeviceStream* stream, DeviceEventPtr* out) = 0;
  virtual Status stream_wait_event(DeviceStream* stream, DeviceEvent* ev) = 0;

  /* Makes transferred data visible to later kernels. A direct RDMA write to
   * device memory establishes no ordering against a consuming kernel by itself. */
  virtual Status make_visible(DeviceStream* stream, void* addr,
                              uint64_t bytes) = 0;
};

}  // namespace hux
#endif  // HUX_DEVICE_H
