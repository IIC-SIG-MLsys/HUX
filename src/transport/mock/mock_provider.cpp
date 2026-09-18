/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
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

  uint32_t taken = 0;
  for (uint32_t i = 0; i < limit; ++i) {
    SubOp const& op = ops[i];
    if (cfg_.budget_bytes != 0 &&
        inflight_bytes_ + op.length > cfg_.budget_bytes) {
      /* Out of budget: the rest is not refused outright, it is offered again
       * later. */
      r.status = Status::kWouldBlock;
      break;
    }
    inflight_bytes_ += op.length;
    ++taken;
    if (cfg_.move_data) {
      /* One-sided transfer simulated in-process: both ends share an address
       * space in tests, so remote_addr can be dereferenced. A real provider
       * never does this. */
      void* dst = op.kind == SubOp::Kind::kRead
                      ? op.local_addr
                      : reinterpret_cast<void*>(op.remote_addr);
      void const* src = op.kind == SubOp::Kind::kRead
                            ? reinterpret_cast<void const*>(op.remote_addr)
                            : op.local_addr;
      std::memcpy(dst, src, static_cast<size_t>(op.length));
    }
    if (op.signal_peer) {
      /* Loops back in-process: both ends of a mock transfer are the same
       * engine, which is enough to exercise the handoff path. */
      arrivals_.push_back(PeerArrival{op.peer_token, 0});
    }
    CompletionEvent ev;
    ev.request = op.request;
    ev.sub_id = op.sub_id;
    ev.bytes = op.length;
    ev.status = cfg_.fail_subops ? cfg_.subop_error : Status::kOk;
    ev.may_have_modified_target =
        cfg_.fail_subops && op.kind == SubOp::Kind::kWrite;
    pending_.push_back(ev);
    ++submitted_;
    ++stats_.subops_posted;
    stats_.payload_bytes += op.length;
    if (cfg_.move_data) {
      /* The mock really does memcpy, so it reports that cost rather than
       * pretending to be zero-copy. A real one-sided transfer adds nothing. */
      stats_.payload_bytes_copied += op.length;
    }
  }
  r.accepted = taken;
  if (taken < ops.size() && r.status == Status::kOk)
    r.status = cfg_.submit_status_on_partial;

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
  /* Hand over the whole batch; never return early on a matching entry. */
  while (!pending_.empty() && out->size() < max_events) {
    out->push_back(pending_.front());
    if (pending_.front().status == Status::kOk)
      ++stats_.subops_completed;
    else
      ++stats_.subops_failed;
    uint64_t const b = pending_.front().bytes;
    inflight_bytes_ -= b < inflight_bytes_ ? b : inflight_bytes_;
    pending_.pop_front();
  }
  return Status::kOk;
}

}  // namespace hux
