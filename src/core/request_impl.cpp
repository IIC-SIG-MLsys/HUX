/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "core/request_impl.h"

#include <chrono>

#include "hux/device.h"

namespace hux {
namespace {
constexpr uint32_t stage_bit(Stage s) { return 1u << static_cast<uint32_t>(s); }
}  // namespace

RequestImpl::RequestImpl(RequestId id, SubOp::Kind kind, uint32_t total_subops,
                         void* context)
    : id_(id), kind_(kind), total_subops_(total_subops), context_(context) {
  stages_ = stage_bit(Stage::kAccepted);
}

bool RequestImpl::terminal_locked() const { return is_terminal(state_); }

RequestState RequestImpl::state() const {
  std::lock_guard<std::mutex> g(mu_);
  return state_;
}

bool RequestImpl::reached(Stage s) const {
  std::lock_guard<std::mutex> g(mu_);
  return (stages_ & stage_bit(s)) != 0;
}

bool RequestImpl::cancel_requested() const {
  std::lock_guard<std::mutex> g(mu_);
  return cancel_requested_;
}

void RequestImpl::set_state(RequestState s) {
  std::lock_guard<std::mutex> g(mu_);
  state_ = s;
}

void RequestImpl::mark_stage(Stage s) {
  std::lock_guard<std::mutex> g(mu_);
  stages_ |= stage_bit(s);
}

void RequestImpl::add_accepted_subops(uint32_t n) {
  std::lock_guard<std::mutex> g(mu_);
  accepted_subops_ += n;
}

void RequestImpl::seal_accepted() {
  std::lock_guard<std::mutex> g(mu_);
  sealed_ = true;
}

bool RequestImpl::sealed() const {
  std::lock_guard<std::mutex> g(mu_);
  return sealed_;
}

uint32_t RequestImpl::accepted_subops() const {
  std::lock_guard<std::mutex> g(mu_);
  return accepted_subops_;
}

void RequestImpl::set_target(void* addr, uint64_t bytes) {
  std::lock_guard<std::mutex> g(mu_);
  target_addr_ = addr;
  target_bytes_ = bytes;
}

bool RequestImpl::on_subop_complete(CompletionEvent const& ev) {
  std::lock_guard<std::mutex> g(mu_);
  if (terminal_locked()) return false;

  if (ev.status != Status::kOk && error_.ok()) {
    error_.status = ev.status;
    error_.provider_errno = ev.provider_errno;
    error_.may_have_modified_target = ev.may_have_modified_target;
  }
  ++completed_subops_;
  /* Measured against the request's full size while more may still be
   * submitted, and against what was accepted once submission has stopped. */
  uint32_t const expected = sealed_ ? accepted_subops_ : total_subops_;
  return completed_subops_ >= expected;
}

Status RequestImpl::test(bool* done) {
  if (done == nullptr) return Status::kInvalidArgument;
  std::lock_guard<std::mutex> g(mu_);
  *done = terminal_locked();
  return Status::kOk;
}

Status RequestImpl::wait(int64_t timeout_ms) {
  std::unique_lock<std::mutex> lk(mu_);
  if (terminal_locked()) return error_.ok() ? Status::kOk : error_.status;

  if (timeout_ms < 0) {
    cv_.wait(lk, [this] { return terminal_locked(); });
  } else {
    bool got = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this] { return terminal_locked(); });
    /* A timeout ends this wait only: the request is still in flight, nothing
     * is cancelled or deregistered, and DMA may well be running. */
    if (!got) return Status::kTimeout;
  }
  if (state_ == RequestState::kCancelled) return Status::kCancelled;
  return error_.ok() ? Status::kOk : error_.status;
}

Status RequestImpl::cancel() {
  std::lock_guard<std::mutex> g(mu_);
  if (state_ == RequestState::kSucceeded) {
    /* The ready handoff is irrevocable; report cancel-too-late honestly. */
    return Status::kInvalidArgument;
  }
  if (terminal_locked()) return Status::kOk;
  cancel_requested_ = true;
  state_ = RequestState::kDraining;
  return Status::kOk;
}

void RequestImpl::fail(ErrorInfo const& e) {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (terminal_locked()) return;
    if (error_.ok()) error_ = e;
    stages_ |= stage_bit(Stage::kFailedSafe);
    state_ = RequestState::kFailed;
  }
  cv_.notify_all();
}

void RequestImpl::finish_success() {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (terminal_locked()) return;
    stages_ |= stage_bit(Stage::kTransferComplete) |
               stage_bit(Stage::kTargetReady) |
               stage_bit(Stage::kSourceReusable);
    state_ = RequestState::kSucceeded;
  }
  cv_.notify_all();
}

void RequestImpl::finish_cancelled() {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (terminal_locked()) return;
    stages_ |= stage_bit(Stage::kCancelledSafe);
    state_ = RequestState::kCancelled;
  }
  cv_.notify_all();
}

Status RequestImpl::wait_on(DeviceStream* stream) {
  if (stream == nullptr) return Status::kInvalidArgument;
  if (device_ == nullptr) return Status::kUnsupported;
  /* Installs the dependency and returns; never blocks on the network. */
  return device_->make_visible(stream, target_addr_, target_bytes_);
}

Status RequestImpl::wait_source_reusable_on(DeviceStream* stream) {
  if (stream == nullptr) return Status::kInvalidArgument;
  if (kind_ != SubOp::Kind::kWrite) return Status::kInvalidArgument;
  if (device_ == nullptr) return Status::kUnsupported;
  return Status::kOk;
}

}  // namespace hux
