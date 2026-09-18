// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_TYPES_H
#define HUX_TYPES_H

#include <cstddef>
#include <cstdint>

namespace hux {

using PeerId = uint64_t;
using RegionId = uint64_t;
using RequestId = uint64_t;
using NotificationId = uint64_t;

// 连接代次。对端重启或重连后 +1，旧描述符和旧请求不得转移到新连接上重试。
using Generation = uint32_t;
using Epoch = uint32_t;

// 设备种类。公共头文件不出现 CUDA/HIP/CNRT 类型，厂商句柄一律封装在 DeviceBackend 内。
enum class DeviceKind : uint8_t {
  kHost = 0,
  kCuda,
  kRocm,      // AMD 与海光 DCU 共用 HIP 运行时，能力差异由 capability 表达
  kCambricon,
  kMoore,
};

// 内存种类。与 DeviceKind 正交：host pinned 内存属于 kHost 设备但需要单独标注，
// 因为它对注册和异步拷贝的要求与普通 pageable 内存不同。
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

// 一段连续范围。offset/length 一律 64 位：要覆盖超过 4 GiB 的内存池和非零高位 offset。
struct Span {
  uint64_t offset = 0;
  uint64_t length = 0;

  // 防溢出的范围检查。先比较再相加，避免 offset+length 回绕后误判为合法。
  bool within(uint64_t total) const {
    return offset <= total && length <= total - offset;
  }
};

}  // namespace hux
#endif  // HUX_TYPES_H
