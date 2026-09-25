/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_TYPES_H
#define HUX_TYPES_H

#include <cstddef>
#include <cstdint>

namespace hux {

using PeerId = uint64_t;
using RegionId = uint64_t;
using RequestId = uint64_t;
using NotificationId = uint64_t;

/* Bumped on reconnect. Old requests are never carried over to a new connection.
 */
using Generation = uint32_t;
using Epoch = uint32_t;

/* Public headers never expose CUDA/HIP/CNRT types; vendor handles stay inside
 * DeviceBackend. */
enum class DeviceKind : uint8_t {
  kHost = 0,
  kCuda,
  kRocm, /* AMD and Hygon DCU share the HIP runtime. */
  kCambricon,
  kMoore,
  kKunlun, /* Kunlunxin XPU. */
};

/* Orthogonal to DeviceKind: pinned host memory differs from pageable in both
 * registration and async copy requirements. */
enum class MemoryKind : uint8_t {
  kHostPageable = 0,
  kHostPinned,
  kDevice,
};

struct DeviceId {
  DeviceKind kind = DeviceKind::kHost;
  int32_t index = 0;

  bool operator==(DeviceId const& o) const {
    return kind == o.kind && index == o.index;
  }
  bool operator!=(DeviceId const& o) const { return !(*this == o); }
};

enum class AccessFlags : uint32_t {
  kNone = 0,
  kLocalRead = 1u << 0,
  kLocalWrite = 1u << 1,
  kRemoteRead = 1u << 2,
  kRemoteWrite = 1u << 3,
};

inline AccessFlags operator|(AccessFlags a, AccessFlags b) {
  return static_cast<AccessFlags>(static_cast<uint32_t>(a) |
                                  static_cast<uint32_t>(b));
}
inline bool has_flag(AccessFlags set, AccessFlags bit) {
  return (static_cast<uint32_t>(set) & static_cast<uint32_t>(bit)) != 0;
}

/* 64-bit throughout, to cover pools above 4 GiB and non-zero high offsets. */
struct Span {
  uint64_t offset = 0;
  uint64_t length = 0;

  /* Compares before adding so that a wrapping offset+length cannot pass. */
  bool within(uint64_t total) const {
    return offset <= total && length <= total - offset;
  }
};

}  // namespace hux
#endif  // HUX_TYPES_H
