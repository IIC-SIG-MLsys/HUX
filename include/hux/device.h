/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_DEVICE_H
#define HUX_DEVICE_H

#include <cstdint>
#include <memory>
#include <vector>

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
 * An unrecorded event is therefore not a generic "wait for the future" signal.
 */
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
  bool supports_graph_capture = false; /* Tracked separately from streams. */
  bool supports_peer_registration = false;
  bool supports_dmabuf_export = false; /* False on Hygon DTK. */
  /* Whether an allocation can be named to another process on this host and
   * mapped there. Being on one host is not enough on its own: host memory the
   * caller allocated itself cannot be exported at all, and two devices from
   * different vendors have no handle they both understand. */
  bool supports_ipc = false;

  /* Largest single registration, 0 if unbounded. */
  uint64_t max_registration_bytes = 0;

  /* Ceiling on everything this process has registered at once, 0 if
   * unbounded. It is a separate limit: on Cambricon MLU each registration up
   * to 256 MiB succeeds on its own, yet four 64 MiB regions already exhaust
   * the quota, so a caller that only checks the single-registration limit
   * still fails once several regions are live. */
  uint64_t max_total_registration_bytes = 0;
};

/* An allocation named so another process on this host can map it. Vendor
 * handles are opaque fixed-size blobs, carried as bytes so nothing above this
 * layer needs the vendor type.
 *
 * It names the allocation, never the span: vendors export what was allocated,
 * so a caller registering the middle of a buffer exports the whole thing and
 * the offset travels with it. An importer that ignored the offset would map
 * the right memory and read the wrong bytes. */
struct IpcHandle {
  std::vector<uint8_t> bytes;
  uint64_t offset = 0; /* Where the exported address sits in the allocation. */
  uint64_t allocation_bytes = 0;
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
   * device memory establishes no ordering against a consuming kernel by itself.
   */
  virtual Status make_visible(DeviceStream* stream, void* addr,
                              uint64_t bytes) = 0;

  /* Copies between two addresses this process can reach, one of which may be
   * a mapping of another process's memory. It is a copy and is counted as
   * one: the IPC path maps rather than transfers, so the bytes still have to
   * be moved by somebody, and a path that hid this would look zero-copy while
   * spending the same bandwidth.
   *
   * Returns only once the bytes are in place, and that is the whole contract.
   * Vendor copy calls do not all promise this -- device-to-device performs no
   * host-side synchronization, and a pageable host source returns once the
   * staging copy is done rather than once the transfer has landed -- so an
   * implementation has to add the wait rather than inherit it. Everything
   * above treats the return as "done": a provider reports the sub-operation
   * complete, the engine reports the request complete, and a peer told the
   * data is ready reads it immediately. */
  virtual Status copy(void* dst, void const* src, uint64_t bytes) {
    (void)dst;
    (void)src;
    (void)bytes;
    return Status::kUnsupported;
  }

  /* The same copy without the wait, to be followed by settle().
   *
   * Only for a caller that controls every reader of those bytes until it
   * settles -- which the IPC path does, because it holds the completions
   * back until then. It exists because waiting once for a batch costs one
   * synchronization instead of one per sub-operation, and on a shared device
   * that difference was measured at four times the latency.
   *
   * The default simply waits, so a backend that has not implemented it is
   * slower and never wrong. */
  virtual Status copy_nowait(void* dst, void const* src, uint64_t bytes) {
    return copy(dst, src, bytes);
  }

  /* Waits for everything started with copy_nowait to be in place. */
  virtual Status settle() { return Status::kOk; }

  /* Names an allocation for another process on this host. Default is a
   * refusal, so a backend that has not implemented it cannot be mistaken for
   * one that can. */
  virtual Status export_ipc(void* addr, uint64_t length, IpcHandle* out) {
    (void)addr;
    (void)length;
    (void)out;
    return Status::kUnsupported;
  }

  /* Maps a handle exported by another process, returning the address of the
   * span the exporter named. Mapping is per process and per allocation --
   * opening one twice is an error on CUDA -- so callers cache rather than
   * reopening. */
  virtual Status import_ipc(IpcHandle const& handle, void** out) {
    (void)handle;
    (void)out;
    return Status::kUnsupported;
  }

  /* Releases a mapping, given any address within it. The exporting process
   * must not free the allocation until every importer has done this: the
   * mapping outlives the handle, and freeing underneath one leaves the
   * importer reading memory that has been handed to something else. */
  virtual Status close_ipc(void* mapped) {
    (void)mapped;
    return Status::kUnsupported;
  }
};

}  // namespace hux
#endif  // HUX_DEVICE_H
