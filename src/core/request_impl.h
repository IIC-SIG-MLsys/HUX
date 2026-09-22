/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_CORE_REQUEST_IMPL_H
#define HUX_CORE_REQUEST_IMPL_H

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "hux/region.h"
#include "hux/request.h"
#include "transport/provider.h"

namespace hux {

class DeviceBackend;

/* Internal state of one logical request. It holds references to its regions
 * and connection until they are safe to release, so dropping the RequestPtr
 * early cannot free memory that is still under DMA. */
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

  /* Internal state transitions below; not part of the public interface. */

  SubOp::Kind kind() const { return kind_; }
  /* Cancellation requested; posted sub-operations still have to drain. */
  bool cancel_requested() const;

  void set_state(RequestState s);
  void mark_stage(Stage s);

  /* Returns true when this was the last outstanding sub-operation.
   * Aggregation counts actual sub-operations; a single signalled WR on one
   * QP proves nothing about the others. */
  bool on_subop_complete(CompletionEvent const& ev);

  /* Accumulated across submissions: a request whose budget ran out is posted
   * in several goes, and each one adds to the total. */
  void add_accepted_subops(uint32_t n);
  uint32_t accepted_subops() const;

  /* No further sub-operations will be submitted, because submission failed
   * rather than merely being deferred. Until this is set, completion is
   * measured against the request's full size; after it, against what was
   * actually accepted -- otherwise a request abandoned halfway would wait for
   * completions that are never coming. */
  void seal_accepted();
  bool sealed() const;
  uint32_t total_subops() const { return total_subops_; }

  /* Records why this request will fail, without ending it. Submission can
   * stop for good while sub-operations it already handed to the NIC are
   * still reading the source: the error is known then, but FailedSafe says
   * local DMA has stopped, and it has not. The completion path ends the
   * request once those drain. */
  void note_error(ErrorInfo const& e);
  void fail(ErrorInfo const& e);
  void finish_success();
  void finish_cancelled();

  void hold_region(MemoryRegionPtr r) { held_regions_.push_back(std::move(r)); }
  void hold_connection(ProviderConnectionPtr c) { held_conn_ = std::move(c); }
  /* Which peer this belongs to. A transfer split across a peer's adapters
   * has chunks on more than one connection, so "did this request go out on
   * the connection that just died" is the wrong question -- the right one is
   * whether it belongs to the peer that just went. */
  void set_peer(PeerId p) { peer_ = p; }
  PeerId peer() const { return peer_; }
  /* The connection this request went out on: a handoff belongs on the same
   * one, not on whichever peer happens to be registered. */
  ProviderConnectionPtr connection() const { return held_conn_; }
  /* And the transport it went out on, for the same reason: a handoff has to
   * travel over the one that reaches this peer, which is not necessarily the
   * engine's first. */
  void set_provider(TransportProvider* p) { provider_ = p; }
  TransportProvider* provider() const { return provider_; }

  void set_device_backend(DeviceBackend* d) { device_ = d; }
  void set_target(void* addr, uint64_t bytes);

  /* Where a write landed on the peer. Sent once the transfer completes, so
   * the target owner can build a dependency on exactly those bytes -- the
   * 32-bit immediate that signals arrival cannot describe them. */
  void set_remote_target(RegionId region, Generation gen, Span span) {
    remote_region_ = region;
    remote_gen_ = gen;
    remote_span_ = span;
  }
  RegionId remote_region() const { return remote_region_; }
  Generation remote_generation() const { return remote_gen_; }
  Span remote_span() const { return remote_span_; }

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
  bool sealed_ = false;
  bool cancel_requested_ = false;
  /* One bit per Stage, so reached() is stable across repeated queries. */
  uint32_t stages_ = 0;
  ErrorInfo error_;

  std::vector<MemoryRegionPtr> held_regions_;
  ProviderConnectionPtr held_conn_;
  PeerId peer_ = 0;
  TransportProvider* provider_ = nullptr;
  DeviceBackend* device_ = nullptr;
  void* target_addr_ = nullptr;
  uint64_t target_bytes_ = 0;
  RegionId remote_region_ = 0;
  Generation remote_gen_ = 0;
  Span remote_span_;
};

using RequestImplPtr = std::shared_ptr<RequestImpl>;

}  // namespace hux
#endif  // HUX_CORE_REQUEST_IMPL_H
