// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_STATUS_H
#define HUX_STATUS_H

#include <cstdint>
#include <string>

namespace hux {

// 公共错误码。故意区分几类语义相近但处置方式完全不同的失败：
//   WOULD_BLOCK  逻辑请求未被接受，没有任何网络副作用，调用方可原样重试。
//   TIMEOUT      仅表示本次等待结束，请求仍在途，DMA 未必停止。
//   UNSUPPORTED  该 provider/设备确实不具备此能力，不是暂时性错误。
// 不要把这三者合并成一个泛化的 ERROR：调用方对它们的正确反应互不相同。
enum class Status : int32_t {
  kOk = 0,
  kWouldBlock,
  kTimeout,
  kCancelled,
  kUnsupported,
  kInvalidArgument,
  kOutOfRange,       // offset/length 超出 region，或整数溢出
  kNotFound,         // peer / region / request 句柄无效
  kStaleGeneration,  // 描述符的 generation 已失效
  kPeerDisconnected,
  kResourceExhausted,
  kDeviceError,
  kTransportError,
  kInternal,
};

char const* to_string(Status s);
inline bool ok(Status s) { return s == Status::kOk; }

// 失败详情。`may_have_modified_target` 是批量传输失败时调用方唯一能依据的东西：
// 批量操作不承诺事务原子性，失败可能已经写了部分目标区域。
struct ErrorInfo {
  Status status = Status::kOk;
  std::string provider;             // "rdma" / "ucx" / "ipc" / "mock"
  uint64_t peer_id = 0;
  int32_t provider_errno = 0;       // 后端原始错误码，便于定位
  bool may_have_modified_target = false;
  std::string detail;

  bool ok() const { return status == Status::kOk; }
};

}  // namespace hux
#endif  // HUX_STATUS_H
