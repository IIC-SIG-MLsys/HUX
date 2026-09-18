// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
//
// 无硬件 provider。它存在的意义不是"假装能传数据"，而是让那些**在真实硬件上
// 极难构造**的情形变成确定性用例：乱序完成、部分提交失败、一批 CQE 里混着
// 多个请求、完成比提交先到等等。这些正是旧实现里出过错的地方。
#ifndef HUX_TRANSPORT_MOCK_PROVIDER_H
#define HUX_TRANSPORT_MOCK_PROVIDER_H

#include <cstring>
#include <deque>
#include <mutex>
#include <map>
#include <random>
#include <vector>

#include "transport/provider.h"

namespace hux {

struct MockConfig {
  uint32_t qp_count = 1;
  uint32_t submit_capacity = 1024;
  // 每次 submit 最多接受这么多个 SubOp；0 表示不限制。
  // 用来构造"部分 post 失败"。
  uint32_t accept_limit = 0;
  Status submit_status_on_partial = Status::kResourceExhausted;
  // 打乱完成事件顺序，验证 core 不依赖 CQE 的到达次序。
  bool shuffle_completions = false;
  // 让指定 sub_id 的完成报错。
  bool fail_subops = false;
  Status subop_error = Status::kTransportError;
  // 真实搬运数据，便于端到端校验；关掉则只走记账路径。
  bool move_data = true;
};

class MockConnection : public ProviderConnection {
 public:
  explicit MockConnection(MockConfig const& c) : cfg_(c) {}
  uint32_t qp_count() const override { return cfg_.qp_count; }
  uint32_t submit_capacity() const override { return cfg_.submit_capacity; }

 private:
  MockConfig cfg_;
};

class MockProvider : public TransportProvider {
 public:
  explicit MockProvider(MockConfig cfg = {}) : cfg_(cfg), rng_(12345) {}

  ProviderCaps caps() const override {
    ProviderCaps c;
    c.name = "mock";
    c.supports_read = true;
    c.supports_write = true;
    c.supports_vector = true;
    c.supports_multi_qp = cfg_.qp_count > 1;
    c.max_sge = 16;
    return c;
  }

  Status register_region(void* addr, uint64_t length, DeviceId, AccessFlags,
                         uint64_t* local_key, uint64_t* remote_key) override {
    std::lock_guard<std::mutex> g(mu_);
    uint64_t key = next_key_++;
    regions_[key] = {addr, length};
    *local_key = key;
    // 故意让 rkey != lkey：旧实现拿 lkey 当 rkey 导出，在两者偶然相等的
    // 设备上能跑通，换一台就坏。这里让它们必然不同，把问题暴露在测试里。
    *remote_key = key + kRemoteKeyOffset;
    return Status::kOk;
  }

  Status deregister_region(uint64_t local_key) override {
    std::lock_guard<std::mutex> g(mu_);
    return regions_.erase(local_key) > 0 ? Status::kOk : Status::kNotFound;
  }

  Status connect(std::vector<uint8_t> const&,
                 ProviderConnectionPtr* out) override {
    *out = std::make_shared<MockConnection>(cfg_);
    return Status::kOk;
  }
  Status disconnect(ProviderConnectionPtr) override { return Status::kOk; }
  Status local_metadata(std::vector<uint8_t>* out) const override {
    *out = {'m', 'o', 'c', 'k'};
    return Status::kOk;
  }

  SubmitResult submit(ProviderConnection*,
                      std::vector<SubOp> const& ops) override;
  Status poll(uint32_t max_events, std::vector<CompletionEvent>* out) override;
  Status flush(ProviderConnection*) override { return Status::kOk; }
  Status drain(ProviderConnection*, int64_t) override { return Status::kOk; }

  // --- 测试辅助 ---
  size_t pending() const {
    std::lock_guard<std::mutex> g(mu_);
    return pending_.size();
  }
  uint64_t submitted_subops() const {
    std::lock_guard<std::mutex> g(mu_);
    return submitted_;
  }

 private:
  static constexpr uint64_t kRemoteKeyOffset = 0x1000000;
  struct Reg { void* addr; uint64_t length; };

  mutable std::mutex mu_;
  MockConfig cfg_;
  std::mt19937 rng_;
  std::map<uint64_t, Reg> regions_;
  std::deque<CompletionEvent> pending_;
  uint64_t next_key_ = 1;
  uint64_t submitted_ = 0;
};

}  // namespace hux
#endif  // HUX_TRANSPORT_MOCK_PROVIDER_H
