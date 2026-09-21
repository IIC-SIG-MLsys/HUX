/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_REGION_H
#define HUX_REGION_H

#include <cstdint>
#include <memory>
#include <vector>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

/* Wire format version. A major mismatch is rejected outright rather than
 * parsed on a best-effort basis. */
/* 2: the descriptor names the engine that exported it. */
constexpr uint16_t kDescriptorMajor = 2;
constexpr uint16_t kDescriptorMinor = 0;

/* One segment of a transfer. Local and remote segments pair up by index and
 * must have equal lengths. */
struct RegionView {
  RegionId region = 0;
  Span span;
};

/* Handle to registered application memory. HUX does not own the memory: the
 * caller keeps the allocation alive until deregistration, and the Python
 * binding holds a reference to the tensor. */
class MemoryRegion {
 public:
  virtual ~MemoryRegion() = default;

  virtual RegionId id() const = 0;
  virtual Generation generation() const = 0;
  virtual void* base() const = 0;
  virtual uint64_t length() const = 0;
  virtual DeviceId device() const = 0;
  virtual MemoryKind memory_kind() const = 0;
  virtual AccessFlags access() const = 0;

  virtual Status view(uint64_t offset, uint64_t length,
                      RegionView* out) const = 0;

  /* Versioned encoding carrying length, permissions, device and generation. */
  virtual Status export_descriptor(std::vector<uint8_t>* out) const = 0;
};

using MemoryRegionPtr = std::shared_ptr<MemoryRegion>;

/* A region exported by a peer. Generation stops software from using a stale
 * descriptor, but cannot stop hardware that already holds the old rkey:
 * reusing the physical memory requires a real drain and revocation first. */
class RemoteRegion {
 public:
  virtual ~RemoteRegion() = default;

  virtual RegionId id() const = 0;
  virtual Generation generation() const = 0;
  virtual uint64_t length() const = 0;
  virtual DeviceId device() const = 0;
  virtual AccessFlags access() const = 0;
  virtual bool valid() const = 0;
  virtual Status view(uint64_t offset, uint64_t length,
                      RegionView* out) const = 0;
};

using RemoteRegionPtr = std::shared_ptr<RemoteRegion>;

/* Per-item result of a batch registration. A partial failure does not lose
 * ownership of the items that succeeded. */
struct RegistrationResult {
  Status status = Status::kOk;
  MemoryRegionPtr region;
};

}  // namespace hux
#endif  // HUX_REGION_H
