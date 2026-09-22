/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_ENGINE_H
#define HUX_ENGINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hux/config.h"
#include "hux/device.h"
#include "hux/notification.h"
#include "hux/peer.h"
#include "hux/region.h"
#include "hux/request.h"
#include "hux/status.h"
#include "hux/types.h"

namespace hux {

struct TransferOptions {
  /* The NIC may touch the buffers only after these events complete. */
  std::vector<DeviceEventPtr> after;

  /* Published once the request reaches target_ready. A failed or cancelled
   * request never sends a success notification. */
  std::vector<uint8_t> notify_payload;
  bool notify = false;

  void* context = nullptr; /* Local only. */
};

/* Observable counters. payload_bytes_copied is the one that settles whether a
 * path is really zero-copy: a transfer in place leaves it at zero, and a path
 * that stages through an intermediate buffer reports what it moved. */
struct EngineStats {
  uint64_t requests_accepted = 0;
  uint64_t requests_succeeded = 0;
  uint64_t requests_failed = 0;
  uint64_t requests_cancelled = 0;
  uint64_t requests_would_block = 0;
  uint64_t submit_deferred = 0;
  uint64_t requests_waiting_on_dependency = 0;
  uint64_t subops_posted = 0;
  uint64_t subops_completed = 0;
  uint64_t subops_failed = 0;
  uint64_t payload_bytes = 0;
  uint64_t payload_bytes_copied = 0;

  /* Registration reuse. A hit rate near zero means ranges are being
   * registered repeatedly, which is expensive and usually a sign the caller
   * is registering per transfer rather than per pool. */
  uint64_t registrations_created = 0;
  uint64_t registrations_reused = 0;
  uint64_t registration_cache_size = 0;

  /* Control plane, kept apart from data: it has its own resources, and
   * counting them together would hide a stalled control channel behind
   * healthy data traffic.
   *
   * The "sent" counters below mean the transport took the message, not that
   * the peer has read it: a control channel never waits on the socket, so an
   * accepted message can still be queued for a peer that is not reading.
   * What is still owed is the provider's to report; what was refused
   * outright is counted here. */
  uint64_t notifications_sent = 0;
  uint64_t notifications_received = 0;
  uint64_t notifications_dropped = 0; /* queue full, so not acknowledged */
  uint64_t ready_handoffs_sent = 0;
  /* Handoffs the transport refused. The write itself succeeded -- the bytes
   * are on the peer -- but the peer was never told, so a consumer waiting
   * for the handoff waits for ever. Counted separately because the local
   * caller has no other way to find out: its request completed. */
  uint64_t ready_handoffs_failed = 0;
  uint64_t ready_handoffs_received = 0;
  /* Notices that a region is going away. A peer that does not get one goes
   * on holding a descriptor for memory that has been taken back, and finds
   * out when the hardware refuses a transfer -- far from here, and with
   * nothing to say why. Counted because deregistering still succeeds
   * locally, so there is no other sign. */
  uint64_t region_invalidates_sent = 0;
  uint64_t region_invalidates_failed = 0;

  /* Acknowledgements this engine could not send back. The peer's notify()
   * then never completes, and back-pressure depends on it learning the
   * truth. */
  uint64_t notification_acks_failed = 0;

  /* Peak in-flight requests, which is what sizing max_inflight_requests
   * needs; the current value says nothing about what the run demanded. */
  uint64_t peak_inflight_requests = 0;
};

/* The engine's own settings, as machine-readable text.
 *
 * Only half the picture: queue pairs, signalling and the congestion
 * controller belong to the provider. Engine::describe() reports both, and is
 * what a run should record. */
std::string describe_config(EngineConfig const& cfg);

/* The engine owns no communication buffer. The application owns its memory and
 * the engine registers and transfers in place -- this is the main departure
 * from the ConnBuffer-centred design. Several engines may coexist in one
 * process with isolated resources. */
class Engine {
 public:
  virtual ~Engine() = default;

  static Status create(EngineConfig const& cfg,
                       std::shared_ptr<DeviceBackend> device,
                       std::unique_ptr<Engine>* out);

  virtual EngineConfig const& config() const = 0;

  /* Registration. Works on memory the application already owns; it is not
   * required to adopt a HUX allocator. */
  virtual Status register_memory(void* addr, uint64_t length,
                                 AccessFlags access, MemoryRegionPtr* out) = 0;
  virtual Status register_memory_batch(
      std::vector<void*> const& addrs, std::vector<uint64_t> const& lengths,
      AccessFlags access, std::vector<RegistrationResult>* out) = 0;
  /* Blocks new submissions first, then drains local use. */
  virtual Status deregister_memory(MemoryRegionPtr region) = 0;

  /* Releases registrations the reuse cache is holding on nobody's behalf, and
   * reports how many went.
   *
   * Deregistering a handle does not do this: the cache exists to keep the
   * registration for the next caller, so the hardware still holds a key to
   * the memory afterwards. That is fine until the memory is about to be freed
   * or unmapped, which is when a caller needs this -- a NIC with a key to
   * reused memory writes into whatever took its place, and on the IPC path
   * the peer keeps its mapping of an allocation about to disappear.
   *
   * It can block: the IPC path waits here for the peer to confirm it
   * unmapped. */
  virtual Status release_cached_registrations(uint32_t* released) = 0;

  /* Peers. Re-adding a valid identity reuses the existing connection. */

  /* The blob from `local_metadata` starts with this, so `add_peer` can tell
   * an engine's metadata from a single provider's dialling blob by looking
   * rather than by trying a parse and seeing whether it fits. Length-prefixed
   * structures can parse by coincidence, and the layouts differ enough that
   * guessing wrong dials the wrong transport with the wrong bytes.
   *
   * A different major is refused. A newer minor is accepted: minor changes
   * only append fields that an older reader stops before. */
  static constexpr uint32_t kMetadataMagic = 0x58554821; /* "!HUX" */
  static constexpr uint16_t kMetadataMajor = 1;
  static constexpr uint16_t kMetadataMinor = 0;

  virtual Status local_metadata(std::vector<uint8_t>* out) const = 0;
  virtual Status add_peer(std::vector<uint8_t> const& metadata,
                          PeerPtr* out) = 0;
  virtual Status remove_peer(PeerPtr peer) = 0;

  /* Transfers, asynchronous by default. The scalar forms are single-segment
   * shortcuts over the same submission path. One vector operation targets one
   * peer; segments pair up by index with equal lengths. */
  virtual Status read(Peer* peer, RegionView const& local,
                      RegionView const& remote, TransferOptions const& opts,
                      RequestPtr* out) = 0;
  virtual Status write(Peer* peer, RegionView const& local,
                       RegionView const& remote, TransferOptions const& opts,
                       RequestPtr* out) = 0;
  virtual Status readv(Peer* peer, std::vector<RegionView> const& local,
                       std::vector<RegionView> const& remote,
                       TransferOptions const& opts, RequestPtr* out) = 0;
  virtual Status writev(Peer* peer, std::vector<RegionView> const& local,
                        std::vector<RegionView> const& remote,
                        TransferOptions const& opts, RequestPtr* out) = 0;

  /* Success means the peer engine queued it -- not that the peer application
   * handled it, and not that any data transfer completed. */
  virtual Status notify(Peer* peer, std::vector<uint8_t> const& payload,
                        RequestPtr* out) = 0;
  virtual Status poll_notifications(uint32_t max_items,
                                    std::vector<Notification>* out) = 0;

  virtual Status poll_completions(uint32_t max_items,
                                  std::vector<RequestPtr>* out) = 0;
  /* Ready handoffs for regions this engine is the write target of. */
  virtual Status poll_ready_events(uint32_t max_items,
                                   std::vector<ReadyEventPtr>* out) = 0;

  virtual Status progress() = 0;
  virtual Status record_event(DeviceStream* stream, DeviceEventPtr* out) = 0;
  virtual EngineStats stats() const = 0;

  /* Everything in effect for this run: the engine's settings and the
   * provider's together. A report from either side alone describes a
   * configuration nobody is running. */
  virtual std::string describe() const = 0;

  /* Stops accepting work and drains. On timeout it keeps the resources and
   * reports the incomplete state; it never destroys objects still under DMA. */
  virtual Status close(int64_t timeout_ms) = 0;
};

}  // namespace hux
#endif  // HUX_ENGINE_H
