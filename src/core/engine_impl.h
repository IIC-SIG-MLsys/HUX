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

#include "core/region_impl.h"
#include "core/request_impl.h"
#include "hux/engine.h"
#include "transport/provider.h"

namespace hux {

class EngineImpl;

class PeerImpl : public Peer {
 public:
  PeerImpl(PeerId id, ProviderConnectionPtr conn, PeerCaps caps,
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

  ProviderConnection* conn() const { return conn_.get(); }
  ProviderConnectionPtr conn_ptr() const { return conn_; }
  /* Bumping the epoch keeps old requests off a new connection. */
  void bump_epoch() {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    connected_.store(false, std::memory_order_release);
  }

 private:
  PeerId const id_;
  ProviderConnectionPtr conn_;
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
             TransportProviderPtr provider);
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

  /* For PeerImpl. */
  TransportProvider* provider() const { return provider_.get(); }

 private:
  Status submit_vector(Peer* peer, std::vector<RegionView> const& local,
                       std::vector<RegionView> const& remote,
                       TransferOptions const& opts, SubOp::Kind kind,
                       RequestPtr* out);
  /* Splits paired segments into SubOps of at most chunk_bytes. */
  Status build_subops(std::vector<RegionView> const& local,
                      std::vector<RegionView> const& remote, SubOp::Kind kind,
                      RequestId req, std::vector<SubOp>* out,
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
  std::shared_ptr<RemoteRegionImpl> find_remote(RegionId id) const;
  void progress_loop();

  EngineConfig cfg_;
  std::shared_ptr<DeviceBackend> device_;
  TransportProviderPtr provider_;

  mutable std::mutex mu_;
  std::unordered_map<RegionId, std::shared_ptr<MemoryRegionImpl>> regions_;
  /* Registrations kept for reuse. Entries stay while any handle references
   * them; beyond that the bound decides how many are held speculatively. */
  std::deque<RegistrationPtr> reg_cache_;
  std::unordered_map<RegionId, std::shared_ptr<RemoteRegionImpl>> remotes_;
  std::unordered_map<PeerId, std::shared_ptr<PeerImpl>> peers_;
  std::unordered_map<RequestId, RequestImplPtr> inflight_;
  std::deque<RequestPtr> completed_;
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
