/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The single contract between core and a transport backend.
 *
 * Core owns logical requests and scheduling policy; a provider owns the actual
 * WR/QP/CQ state. They meet only over submittable budget, accepted
 * sub-operations, and completion/failure events -- chunking, retry and
 * congestion control are not implemented twice, once on each side. */
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

class ProviderConnection;

/* A control message from a peer. Kept apart from data completions: control
 * traffic has its own resources so a stalled transfer cannot starve the
 * message that would explain it. */
struct ControlMessage {
  PeerId peer = 0;
  uint16_t type = 0;
  std::vector<uint8_t> payload;
  /* The connection it arrived on. A reply belongs on the same one, and a
   * passively accepted connection has no Peer object to look up -- the engine
   * would otherwise have nowhere to send an acknowledgement. */
  ProviderConnection* conn = nullptr;
};

/* Something the peer wrote into this engine's memory, surfaced by the
 * provider once its arrival was signalled. */
struct PeerArrival {
  uint32_t token = 0;
  PeerId from = 0;
};

/* What a provider has actually done. payload_bytes_copied is the one that
 * decides whether a path is zero-copy: a claim in a README cannot be checked,
 * a counter can. A provider that stages through an intermediate buffer adds
 * the bytes it moved, so the cost shows up rather than hiding in the
 * bandwidth figure. */
struct ProviderStats {
  uint64_t subops_posted = 0;
  uint64_t subops_completed = 0;
  uint64_t subops_failed = 0;
  uint64_t payload_bytes = 0;         /* requested by the caller */
  uint64_t payload_bytes_copied = 0;  /* extra copies on top of that */
};

/* Reported as-is. A missing capability returns false rather than being faked
 * with a silent sync or an extra copy. */
struct ProviderCaps {
  std::string name;
  bool supports_read = false;
  bool supports_write = false;
  bool supports_vector = false;      /* Otherwise core splits into scalars. */
  bool supports_multi_qp = false;
  bool needs_explicit_flush = false; /* UCX: local put != remote visibility. */
  /* Whether a write can tell the peer it landed. Without it the peer cannot
   * hand the data to a consumer, and a write cannot honestly reach
   * target_ready. */
  bool supports_peer_signal = false;
  uint64_t max_segment_bytes = 0;    /* 0 if unbounded. */
  uint32_t max_sge = 1;
};

/* A sub-operation handed down by core. Chunking and admission are already
 * done; the provider only turns this into WRs and posts them. */
struct SubOp {
  enum class Kind : uint8_t { kRead, kWrite };
  /* Set on the last sub-operation of a write so the peer learns the data
   * arrived. A one-sided write is invisible to the receiving CPU otherwise,
   * which is why a write cannot reach target_ready on the sender's word
   * alone. */
  bool signal_peer = false;
  uint32_t peer_token = 0;  /* carried to the peer with that signal */
  Kind kind = Kind::kRead;
  RequestId request = 0;
  uint64_t sub_id = 0;      /* Unique within the request; used to aggregate. */
  void* local_addr = nullptr;
  uint64_t local_key = 0;   /* Provider-private registration handle. */
  uint64_t remote_addr = 0;
  uint64_t remote_key = 0;  /* The peer's rkey, never a local lkey. */
  uint64_t length = 0;
};

/* What a provider reports back.
 *
 * There is deliberately no target_ready here: the strongest claim a provider
 * can make is that its own completion condition is met. Device visibility is
 * the DeviceBackend's job and the cross-node ready handoff is the control
 * layer's, so no single CQE can stand in for a strong completion. */
struct CompletionEvent {
  RequestId request = 0;
  uint64_t sub_id = 0;
  Status status = Status::kOk;
  int32_t provider_errno = 0;
  bool may_have_modified_target = false;
};

/* On a partial submit, core needs to know exactly how many were accepted so it
 * can roll back only the rest and keep the budget for what was taken. */
struct SubmitResult {
  uint32_t accepted = 0;
  Status status = Status::kOk;
  int32_t provider_errno = 0;
};

class ProviderConnection {
 public:
  virtual ~ProviderConnection() = default;
  virtual uint32_t qp_count() const = 0;
  /* Room left for WRs; core admits against this to avoid filling the SQ. */
  virtual uint32_t submit_capacity() const = 0;
};

using ProviderConnectionPtr = std::shared_ptr<ProviderConnection>;

class TransportProvider {
 public:
  virtual ~TransportProvider() = default;

  virtual ProviderCaps caps() const = 0;
  virtual ProviderStats stats() const = 0;

  virtual Status register_region(void* addr, uint64_t length, DeviceId device,
                                 AccessFlags access, uint64_t* local_key,
                                 uint64_t* remote_key) = 0;
  virtual Status deregister_region(uint64_t local_key) = 0;

  virtual Status connect(std::vector<uint8_t> const& peer_metadata,
                         ProviderConnectionPtr* out) = 0;
  virtual Status disconnect(ProviderConnectionPtr conn) = 0;
  virtual Status local_metadata(std::vector<uint8_t>* out) const = 0;

  /* Accepts as many ops as it can, in order, and reports how far it got. */
  virtual SubmitResult submit(ProviderConnection* conn,
                              std::vector<SubOp> const& ops) = 0;

  /* Must hand over every event it took from the CQ. Returning early on a
   * matching entry drops the completions of other requests in the same batch. */
  virtual Status poll(uint32_t max_events,
                      std::vector<CompletionEvent>* out) = 0;

  /* Sends on the control channel. Serialized per connection: a header and its
   * payload interleaved with another message would desynchronize the peer's
   * reader for good. */
  virtual Status send_control(ProviderConnection* conn, uint16_t type,
                              std::vector<uint8_t> const& payload) = 0;

  /* Control messages received since the last call. */
  virtual Status poll_control(uint32_t max_items,
                              std::vector<ControlMessage>* out) = 0;

  /* Arrivals signalled by peers since the last call. Empty for providers that
   * cannot signal, which report it through caps. */
  virtual Status poll_peer_arrivals(uint32_t max_items,
                                    std::vector<PeerArrival>* out) = 0;

  /* Required when needs_explicit_flush; others may return kOk. */
  virtual Status flush(ProviderConnection* conn) = 0;

  virtual Status drain(ProviderConnection* conn, int64_t timeout_ms) = 0;
};

using TransportProviderPtr = std::shared_ptr<TransportProvider>;

}  // namespace hux
#endif  // HUX_TRANSPORT_PROVIDER_H
