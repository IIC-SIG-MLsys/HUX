// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_PEER_H
#define HUX_PEER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hux/region.h"
#include "hux/status.h"
#include "hux/types.h"

namespace hux {

// 实际选中的传输路径。必须可查询——调用方需要知道自己拿到的是直连还是降级路径，
// 否则"能力失败"会被静默的性能下降掩盖。
enum class PathKind : uint8_t {
  kUnknown = 0,
  kSameProcess,  // 同进程，直接访问本地地址
  kIpc,          // 同机跨进程
  kRdma,
  kUcx,
};

char const* to_string(PathKind p);

// 建连时协商出的对端能力。
struct PeerCaps {
  PathKind path = PathKind::kUnknown;
  std::string provider;
  uint32_t qp_count = 0;
  bool remote_device_is_gpu = false;
  // 对端单次注册上限（0 = 无已知限制）。跨厂商时两端限制都要考虑。
  uint64_t remote_max_registration_bytes = 0;
};

// 稳定的对端句柄。连接身份不再泄漏到每次请求里——旧设计每次 put/get 都要传
// IP/port/ConnType，导致连接选择与数据路径耦合。
class Peer {
 public:
  virtual ~Peer() = default;

  virtual PeerId id() const = 0;
  // 连接代次。断连重连后递增，旧请求不会被转移到新连接上重试。
  virtual Epoch epoch() const = 0;
  virtual PeerCaps caps() const = 0;
  virtual bool connected() const = 0;

  // 导入对端导出的区域描述符。校验版本、长度、权限、设备与 generation。
  virtual Status import_region(std::vector<uint8_t> const& descriptor,
                               RemoteRegionPtr* out) = 0;
  virtual Status import_region_batch(
      std::vector<std::vector<uint8_t>> const& descriptors,
      std::vector<RemoteRegionPtr>* out) = 0;
};

using PeerPtr = std::shared_ptr<Peer>;

}  // namespace hux
#endif  // HUX_PEER_H
