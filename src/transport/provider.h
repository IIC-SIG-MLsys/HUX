// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
//
// TransportProvider：core 与具体传输后端之间的唯一契约。
//
// 设计要点：core 维护逻辑请求与调度策略，provider 维护 WR/QP/CQ 这类实际状态。
// 两者只通过"可提交预算、已接受子操作、完成/失败事件"三样东西连接——
// 不在两层各实现一套分片、重试和拥塞控制。
#ifndef HUX_TRANSPORT_PROVIDER_H
#define HUX_TRANSPORT_PROVIDER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hux/peer.h"
#include "hux/region.h"
#include "hux/status.h"
#include "hux/types.h"

namespace hux {

// provider 能力。core 依据它决定能否满足调用方的请求，
// 不具备的能力如实返回 false，而不是用静默同步或额外拷贝冒充。
struct ProviderCaps {
  std::string name;
  bool supports_read = false;
  bool supports_write = false;
  bool supports_vector = false;      // 原生向量操作；否则由 core 拆成多个标量
  bool supports_multi_qp = false;
  bool needs_explicit_flush = false; // UCX 的 put 本地完成 != 远端可见
  uint64_t max_segment_bytes = 0;    // 0 = 无限制
  uint32_t max_sge = 1;
};

// core 提交给 provider 的一个子操作。core 已完成分片与准入，
// provider 不再自行拆分，只负责把它变成实际的 WR 并提交。
struct SubOp {
  enum class Kind : uint8_t { kRead, kWrite };
  Kind kind = Kind::kRead;
  RequestId request = 0;
  uint64_t sub_id = 0;      // 同一 request 内唯一，用于完成聚合
  void* local_addr = nullptr;
  uint64_t local_key = 0;   // provider 私有的注册句柄
  uint64_t remote_addr = 0;
  uint64_t remote_key = 0;  // 必须是对端导出的 rkey，不能用本地 lkey 顶替
  uint64_t length = 0;
};

// provider 上报的完成事件。core 据此聚合父请求。
//
// 注意这里只有 kTransferComplete 而没有 target_ready：provider 能证明的
// 最强结论就是"该方向的传输完成条件已满足"。目标可见性由 DeviceBackend 处理，
// 跨节点的 ready 交接由 control 层回报。不能让某个 CQE 直接冒充强完成。
struct CompletionEvent {
  RequestId request = 0;
  uint64_t sub_id = 0;
  Status status = Status::kOk;
  int32_t provider_errno = 0;
  bool may_have_modified_target = false;
};

// 提交结果。部分提交失败时，core 需要知道**实际被接受到第几个**，
// 才能只回滚未接受的部分、保留已接受部分的引用和预算。
struct SubmitResult {
  uint32_t accepted = 0;  // 前 accepted 个 SubOp 已被接受
  Status status = Status::kOk;
  int32_t provider_errno = 0;
};

// provider 侧的连接。
class ProviderConnection {
 public:
  virtual ~ProviderConnection() = default;
  virtual uint32_t qp_count() const = 0;
  // 当前还能接受多少个 WR。core 用它做准入，避免把 SQ 填满。
  virtual uint32_t submit_capacity() const = 0;
};

using ProviderConnectionPtr = std::shared_ptr<ProviderConnection>;

class TransportProvider {
 public:
  virtual ~TransportProvider() = default;

  virtual ProviderCaps caps() const = 0;

  // 注册应用已有内存，返回 provider 私有的句柄与可导出的远端 key。
  virtual Status register_region(void* addr, uint64_t length, DeviceId device,
                                 AccessFlags access, uint64_t* local_key,
                                 uint64_t* remote_key) = 0;
  virtual Status deregister_region(uint64_t local_key) = 0;

  virtual Status connect(std::vector<uint8_t> const& peer_metadata,
                         ProviderConnectionPtr* out) = 0;
  virtual Status disconnect(ProviderConnectionPtr conn) = 0;
  virtual Status local_metadata(std::vector<uint8_t>* out) const = 0;

  // 批量提交。provider 按顺序尽量多地接受，并如实报告接受到第几个。
  virtual SubmitResult submit(ProviderConnection* conn,
                              std::vector<SubOp> const& ops) = 0;

  // 取出已产生的完成事件。**必须把整批取到的事件全部交出来**——
  // 旧实现里"取得一批 CQE 后遇到目标即返回"会丢掉同批其他请求的完成。
  virtual Status poll(uint32_t max_events,
                      std::vector<CompletionEvent>* out) = 0;

  // needs_explicit_flush 为 true 的 provider 必须实现；其余可返回 kOk。
  virtual Status flush(ProviderConnection* conn) = 0;

  // 停止接收新提交并等待已提交部分安全结束。
  virtual Status drain(ProviderConnection* conn, int64_t timeout_ms) = 0;
};

using TransportProviderPtr = std::shared_ptr<TransportProvider>;

}  // namespace hux
#endif  // HUX_TRANSPORT_PROVIDER_H
