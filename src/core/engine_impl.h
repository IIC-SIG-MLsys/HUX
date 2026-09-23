/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_CORE_ENGINE_IMPL_H
#define HUX_CORE_ENGINE_IMPL_H

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "control/identity.h"
#include "core/region_impl.h"
#include "core/request_impl.h"
#include "hux/engine.h"
#include "transport/provider.h"

namespace hux {

class EngineImpl;

class PeerImpl : public Peer {
 public:
  /* One way of reaching this peer. A peer on a host with an adapter each
   * side of it has one of these per adapter, and a transfer can be split
   * between them -- which is worth doing only because they do not share a
   * bottleneck, and worth weighting because they are rarely equal. */
  struct Lane {
    TransportProviderPtr provider;
    ProviderConnectionPtr conn;
    /* Share of a transfer's bytes. Measured rather than assumed: two
     * adapters on one host here differ by 2.14x, and an even split would be
     * held to the slower one. */
    double weight = 1.0;
  };

  PeerImpl(PeerId id, Identity remote, std::vector<Lane> lanes, PeerCaps caps,
           EngineImpl* engine);

  PeerId id() const override { return id_; }
  Epoch epoch() const override {
    return epoch_.load(std::memory_order_acquire);
  }
  PeerCaps caps() const override { return caps_; }
  bool connected() const override {
    return connected_.load(std::memory_order_acquire);
  }
  Status import_region(std::vector<uint8_t> const& descriptor,
                       RemoteRegionPtr* out) override;
  Status import_region_batch(std::vector<std::vector<uint8_t>> const& descs,
                             std::vector<RemoteRegionPtr>* out) override;

  /* The first lane. Control messages travel on it rather than being spread:
   * an invalidation or a notification is one message, and splitting it
   * between adapters would only make its ordering harder to reason about. */
  ProviderConnection* conn() const {
    return lanes_.empty() ? nullptr : lanes_[0].conn.get();
  }
  ProviderConnectionPtr conn_ptr() const {
    return lanes_.empty() ? nullptr : lanes_[0].conn;
  }
  std::vector<Lane> const& lanes() const { return lanes_; }
  /* A peer is gone when any way of reaching it is: a transfer split across
   * lanes cannot complete on the strength of the ones still up. */
  bool any_lane_gone() const {
    for (auto const& l : lanes_)
      if (l.conn != nullptr && !l.conn->alive()) return true;
    return false;
  }
  /* The provider this peer was reached over. Held per peer rather than per
   * engine: a peer in the next process and a peer on the next machine are
   * reached by different transports, and a request has to go out over the one
   * that can actually reach its peer. */
  TransportProvider* provider() const {
    return lanes_.empty() ? nullptr : lanes_[0].provider.get();
  }
  /* Bumping the epoch keeps old requests off a new connection. */
  void bump_epoch() {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    connected_.store(false, std::memory_order_release);
  }

 private:
  PeerId const id_;
  /* Who is on the other end, as the metadata said. Held so an imported
   * descriptor can be checked against the peer it is being imported into. */
  Identity const remote_identity_;
  std::vector<Lane> lanes_;
  PeerCaps const caps_;
  EngineImpl* const engine_;
  std::atomic<Epoch> epoch_{0};
  std::atomic<bool> connected_{true};
  std::mutex mu_;
  std::vector<RemoteRegionPtr> imported_;
};

class EngineImpl : public Engine {
 public:
  EngineImpl(EngineConfig cfg, std::shared_ptr<DeviceBackend> device,
             std::vector<TransportProviderPtr> providers);
  ~EngineImpl() override;

  EngineConfig const& config() const override { return cfg_; }

  Status register_memory(void* addr, uint64_t length, AccessFlags access,
                         MemoryRegionPtr* out) override;
  Status register_memory_batch(std::vector<void*> const& addrs,
                               std::vector<uint64_t> const& lengths,
                               AccessFlags access,
                               std::vector<RegistrationResult>* out) override;
  Status deregister_memory(MemoryRegionPtr region) override;
  Status release_cached_registrations(uint32_t* released) override;

  Status local_metadata(std::vector<uint8_t>* out) const override;
  Status add_peer(std::vector<uint8_t> const& metadata, PeerPtr* out) override;
  Status remove_peer(PeerPtr peer) override;

  Status read(Peer* peer, RegionView const& local, RegionView const& remote,
              TransferOptions const& opts, RequestPtr* out) override;
  Status write(Peer* peer, RegionView const& local, RegionView const& remote,
               TransferOptions const& opts, RequestPtr* out) override;
  Status readv(Peer* peer, std::vector<RegionView> const& local,
               std::vector<RegionView> const& remote,
               TransferOptions const& opts, RequestPtr* out) override;
  Status writev(Peer* peer, std::vector<RegionView> const& local,
                std::vector<RegionView> const& remote,
                TransferOptions const& opts, RequestPtr* out) override;

  Status notify(Peer* peer, std::vector<uint8_t> const& payload,
                RequestPtr* out) override;
  Status poll_notifications(uint32_t max_items,
                            std::vector<Notification>* out) override;
  Status poll_completions(uint32_t max_items,
                          std::vector<RequestPtr>* out) override;
  Status poll_ready_events(uint32_t max_items,
                           std::vector<ReadyEventPtr>* out) override;
  Status progress() override;
  Status record_event(DeviceStream* stream, DeviceEventPtr* out) override;
  EngineStats stats() const override;
  std::string describe() const override;
  Status close(int64_t timeout_ms) override;

  /* For PeerImpl. The first provider, which is the only one when there is
   * only one. */
  TransportProvider* provider() const { return providers_.front().get(); }

 private:
  Status submit_vector(Peer* peer, std::vector<RegionView> const& local,
                       std::vector<RegionView> const& remote,
                       TransferOptions const& opts, SubOp::Kind kind,
                       RequestPtr* out);
  /* Splits paired segments into SubOps of at most chunk_bytes. */
  Status build_subops(PeerId peer, std::vector<RegionView> const& local,
                      std::vector<RegionView> const& remote, SubOp::Kind kind,
                      RequestId req, std::vector<PeerImpl::Lane> const& lanes,
                      std::vector<std::vector<SubOp>>* out,
                      std::vector<MemoryRegionPtr>* held, void** target_addr,
                      uint64_t* target_bytes);
  /* A request whose device dependencies have not been met yet. It is admitted
   * and returned to the caller immediately -- submission is what waits, not
   * the calling thread. */
  struct PendingSubmit {
    RequestImplPtr req;
    std::vector<SubOp> ops;
    ProviderConnectionPtr conn;
    PeerId peer = 0;
    /* The peer's provider, carried with the work: by the time a deferred
     * request is submitted, looking the peer up again would be a second
     * chance to pick the wrong one. */
    TransportProvider* provider = nullptr;
    std::vector<DeviceEventPtr> after;
  };

  /* Returns true once every event has been recorded and completed. An
   * unrecorded event captures no work, so treating it as satisfied would
   * release the NIC against data that does not exist yet. */
  bool dependencies_met(std::vector<DeviceEventPtr> const& after) const;
  /* Submits at most max_bytes worth of sub-operations, leaving the rest for a
   * later turn. Returns true when the request has nothing left to submit. */
  bool post_ops(PendingSubmit* p, uint64_t max_bytes);
  void drain_pending();
  /* An existing registration that covers this request, or nullptr. */
  RegistrationPtr find_registration(void* addr, uint64_t length,
                                    DeviceId device, AccessFlags access);
  void cache_registration(RegistrationPtr reg);
  std::shared_ptr<MemoryRegionImpl> find_region(RegionId id) const;
  /* Keyed by the peer as well as the id. Region ids are handed out per
   * engine from one, so every peer has a region 1 and an id alone names
   * nothing. */
  PeerId peer_for_conn(ProviderConnection* conn) const;
  std::shared_ptr<RemoteRegionImpl> find_remote(PeerId peer, RegionId id) const;
  void progress_loop();
  /* Notices peers whose connection has gone and settles what was in flight to
   * them. A peer that exits between transfers leaves nothing to fail on its
   * own, so without this a request submitted afterwards waits for completions
   * that are never coming. */
  void reap_departed_peers();

  /* Chooses the provider for a peer from where that peer is. Returns null
   * when nothing here can reach it, which is a refusal, not a fallback: a
   * transfer quietly taking a slower path than the caller was told is worse
   * than one that does not start. */
  TransportProviderPtr provider_for(Locality locality) const;

  EngineConfig cfg_;
  std::shared_ptr<DeviceBackend> device_;
  /* In preference order. The first is what a caller gets when nothing more
   * specific applies. */
  std::vector<TransportProviderPtr> providers_;

  mutable std::mutex mu_;
  std::unordered_map<RegionId, std::shared_ptr<MemoryRegionImpl>> regions_;
  /* Registrations kept for reuse. Entries stay while any handle references
   * them; beyond that the bound decides how many are held speculatively. */
  std::deque<RegistrationPtr> reg_cache_;
  std::map<std::pair<PeerId, RegionId>, std::shared_ptr<RemoteRegionImpl>>
      remotes_;
  std::unordered_map<PeerId, std::shared_ptr<PeerImpl>> peers_;
  std::unordered_map<RequestId, RequestImplPtr> inflight_;
  std::deque<RequestPtr> completed_;
  /* Both called with mu_ held. The first returns whatever it pushed off the
   * front, for the caller to release once mu_ is not held: a finished
   * request can hold the last reference to a region, and releasing that
   * deregisters it. */
  RequestPtr keep_completed_locked(RequestPtr req);
  void keep_ready_locked(ReadyEventPtr ev);
  std::atomic<uint64_t> completions_dropped_{0};
  std::atomic<uint64_t> ready_events_dropped_{0};
  std::deque<PendingSubmit> pending_;
  /* Where the next scheduling pass starts, so no request is permanently
   * first. */
  size_t rr_cursor_ = 0;
  std::deque<Notification> notifications_;
  std::deque<ReadyEventPtr> ready_events_;
  /* Notification requests waiting for the peer to confirm receipt, keyed by
   * the id carried out and back. */
  std::unordered_map<uint64_t, RequestImplPtr> notify_pending_;
  std::atomic<uint64_t> next_notify_{1};

  std::atomic<uint64_t> next_region_{1};
  std::atomic<uint64_t> next_peer_{1};
  std::atomic<uint64_t> next_request_{1};
  std::atomic<uint32_t> generation_{1};
  /* Distinguishes this engine from others in the same process, so two of them
   * do not mistake each other for themselves. */
  uint64_t const engine_id_;

  mutable std::mutex stats_mu_;
  EngineStats stats_;

  std::thread progress_thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> closed_{false};

  friend class PeerImpl;
};

}  // namespace hux
#endif  // HUX_CORE_ENGINE_IMPL_H
