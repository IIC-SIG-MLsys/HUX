// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_CORE_REQUEST_IMPL_H
#define HUX_CORE_REQUEST_IMPL_H

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "hux/request.h"
#include "hux/region.h"
#include "transport/provider.h"

namespace hux {

class DeviceBackend;

// 一次逻辑请求的内部状态。
//
// 它持有 Region 与连接的引用直到安全释放为止：调用方提前丢掉 RequestPtr
// 不会让底层资源在 DMA 还在进行时被回收。
class RequestImpl : public Request {
 public:
  RequestImpl(RequestId id, SubOp::Kind kind, uint32_t total_subops,
              void* context);

  RequestId id() const override { return id_; }
  RequestState state() const override;
  bool reached(Stage s) const override;
  Status test(bool* done) override;
  Status wait(int64_t timeout_ms) override;
  Status cancel() override;
  Status wait_on(DeviceStream* stream) override;
  Status wait_source_reusable_on(DeviceStream* stream) override;
  ErrorInfo const& error() const override { return error_; }
  void* context() const override { return context_; }

  // --- 以下供 core 内部推进状态，不属于公共接口 ---

  SubOp::Kind kind() const { return kind_; }
  // 是否已被取消请求过。已提交的子操作仍需 drain。
  bool cancel_requested() const;

  void set_state(RequestState s);
  void mark_stage(Stage s);

  // 记录一个子操作完成。返回 true 表示这是最后一个未决子操作。
  // **完成聚合必须按实际子操作计数**，不能凭某一条 QP 上的单个 signal 推断整体。
  bool on_subop_complete(CompletionEvent const& ev);

  // 记录已被 provider 接受的子操作数。部分提交失败时只回滚未接受部分。
  void set_accepted_subops(uint32_t n);
  uint32_t accepted_subops() const;
  uint32_t total_subops() const { return total_subops_; }

  void fail(ErrorInfo const& e);
  void finish_success();
  void finish_cancelled();

  void hold_region(MemoryRegionPtr r) { held_regions_.push_back(std::move(r)); }
  void hold_connection(ProviderConnectionPtr c) { held_conn_ = std::move(c); }

  void set_device_backend(DeviceBackend* d) { device_ = d; }
  void set_target(void* addr, uint64_t bytes);

 private:
  bool terminal_locked() const;

  mutable std::mutex mu_;
  std::condition_variable cv_;

  RequestId const id_;
  SubOp::Kind const kind_;
  uint32_t const total_subops_;
  void* const context_;

  RequestState state_ = RequestState::kQueued;
  uint32_t completed_subops_ = 0;
  uint32_t accepted_subops_ = 0;
  bool cancel_requested_ = false;
  // 位图：每个 Stage 一位。reached() 查它，保证重复查询返回一致结果。
  uint32_t stages_ = 0;
  ErrorInfo error_;

  std::vector<MemoryRegionPtr> held_regions_;
  ProviderConnectionPtr held_conn_;
  DeviceBackend* device_ = nullptr;
  void* target_addr_ = nullptr;
  uint64_t target_bytes_ = 0;
};

using RequestImplPtr = std::shared_ptr<RequestImpl>;

}  // namespace hux
#endif  // HUX_CORE_REQUEST_IMPL_H
