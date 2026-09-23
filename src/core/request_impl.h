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

  /* How many of this request's lanes still have work to post. A request
   * split across adapters is posted lane by lane, and one lane failing must
   * not end the request while another could still post: ending it publishes
   * FailedSafe, and a lane posted afterwards would have the NIC reading a
   * source the caller was just told it may reuse. */
  void set_parts(uint32_t n);
  /* One lane has nothing left to post -- all of it went out, or it was
   * given up. True when it was the last. */
  bool part_finished();
  /* The request has failed, or been cancelled, or ended: whatever of it has
   * not been posted yet should not be. */
  bool abandoning() const;
  /* For once no lane will post again and the request cannot succeed. Seals
   * it, and says whether nothing is outstanding either -- in which case no
   * completion is coming to end it, and the caller has to. Atomic with
   * on_subop_complete, so exactly one of the two ends the request. */
  bool seal_if_idle();
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
  /* Every lane the request went out on, not only the first. Only the peer
   * held the others, so removing and dropping a peer destroyed their queue
   * pairs with this request's work still on them: nothing completed it,
   * and nothing ever ended it. */
  void hold_lane(ProviderConnectionPtr c) {
    held_lanes_.push_back(std::move(c));
  }
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
   * 32-bit immediate that signals arrival cannot describe them. One range
   * per place a vector write landed, with ranges that follow on in the same
   * region merged: a single range, the first segment's offset with every
   * segment's length, named bytes that were never written and left out the
   * other regions altogether. */
  struct RemoteTarget {
    RegionId region = 0;
    Generation generation = 0;
    Span span;
  };
  void add_remote_target(RegionId region, Generation gen, Span span) {
    if (!remote_targets_.empty()) {
      RemoteTarget& last = remote_targets_.back();
      if (last.region == region && last.generation == gen &&
          last.span.offset + last.span.length == span.offset) {
        last.span.length += span.length;
        return;
      }
    }
    remote_targets_.push_back(RemoteTarget{region, gen, span});
  }
  std::vector<RemoteTarget> const& remote_targets() const {
    return remote_targets_;
  }

 private:
  bool terminal_locked() const;
  /* What wait() returns for a request that has ended. */
  Status outcome_locked() const;
  /* kOk once the request has reached s. Before that nothing is installed and
   * the answer is kWouldBlock: ordering device work behind a transfer still
   * on the network needs a stream-side wait, which is not implemented, and
   * answering kOk let a caller launch a kernel that read -- or overwrote --
   * a buffer the adapter was still moving. A request that ended without
   * reaching s says why. */
  Status reached_or(Stage s) const;

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
  uint32_t parts_left_ = 0;
  bool cancel_requested_ = false;
  /* One bit per Stage, so reached() is stable across repeated queries. */
  uint32_t stages_ = 0;
  ErrorInfo error_;

  std::vector<MemoryRegionPtr> held_regions_;
  /* Declared after the regions so they go first: a connection's teardown
   * reaches its provider, which the regions' registrations keep alive. */
  ProviderConnectionPtr held_conn_;
  std::vector<ProviderConnectionPtr> held_lanes_;
  PeerId peer_ = 0;
  TransportProvider* provider_ = nullptr;
  DeviceBackend* device_ = nullptr;
  void* target_addr_ = nullptr;
  uint64_t target_bytes_ = 0;
  std::vector<RemoteTarget> remote_targets_;
};

using RequestImplPtr = std::shared_ptr<RequestImpl>;

}  // namespace hux
#endif  // HUX_CORE_REQUEST_IMPL_H
