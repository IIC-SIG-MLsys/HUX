// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#include "transport/mock/mock_provider.h"

#include <algorithm>

namespace hux {

SubmitResult MockProvider::submit(ProviderConnection*,
                                  std::vector<SubOp> const& ops) {
  std::lock_guard<std::mutex> g(mu_);
  SubmitResult r;
  uint32_t const limit =
      cfg_.accept_limit == 0
          ? static_cast<uint32_t>(ops.size())
          : std::min<uint32_t>(cfg_.accept_limit,
                               static_cast<uint32_t>(ops.size()));

  for (uint32_t i = 0; i < limit; ++i) {
    SubOp const& op = ops[i];
    if (cfg_.move_data) {
      // 在同一进程内模拟单边传输：remote_addr 是对端"虚拟地址"，
      // 测试里两端在同一地址空间，因此可以直接搬。真实 provider 不会这样做。
      void* dst = op.kind == SubOp::Kind::kRead
                      ? op.local_addr
                      : reinterpret_cast<void*>(op.remote_addr);
      void const* src = op.kind == SubOp::Kind::kRead
                            ? reinterpret_cast<void const*>(op.remote_addr)
                            : op.local_addr;
      std::memcpy(dst, src, static_cast<size_t>(op.length));
    }
    CompletionEvent ev;
    ev.request = op.request;
    ev.sub_id = op.sub_id;
    ev.status = cfg_.fail_subops ? cfg_.subop_error : Status::kOk;
    ev.may_have_modified_target =
        cfg_.fail_subops && op.kind == SubOp::Kind::kWrite;
    pending_.push_back(ev);
    ++submitted_;
  }
  r.accepted = limit;
  r.status = limit < ops.size() ? cfg_.submit_status_on_partial : Status::kOk;

  if (cfg_.shuffle_completions && pending_.size() > 1) {
    std::vector<CompletionEvent> v(pending_.begin(), pending_.end());
    std::shuffle(v.begin(), v.end(), rng_);
    pending_.assign(v.begin(), v.end());
  }
  return r;
}

Status MockProvider::poll(uint32_t max_events,
                          std::vector<CompletionEvent>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  std::lock_guard<std::mutex> g(mu_);
  // 整批交出：取到多少给多少，不做"遇到某个目标就提前返回"。
  while (!pending_.empty() && out->size() < max_events) {
    out->push_back(pending_.front());
    pending_.pop_front();
  }
  return Status::kOk;
}

}  // namespace hux
