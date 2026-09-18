/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Native RDMA provider, single queue pair. Multi-QP, batching and congestion
 * control come later; what is fixed here is the accounting those depend on.
 *
 * Three rules are structural rather than incidental, because each one is a
 * place a transport is known to go wrong:
 *
 *   - poll() hands back every completion it drew from the CQ. Returning early
 *     on a match loses the completions of other requests in the same batch.
 *   - a failed work request produces a completion carrying its reason, so a
 *     request can reach a terminal state. A failure that merely stops
 *     reporting leaves the caller unable to tell whether DMA has stopped.
 *   - submit() reports how many operations were accepted, so a partial post
 *     rolls back only what was refused. */
#ifndef HUX_TRANSPORT_RDMA_PROVIDER_H
#define HUX_TRANSPORT_RDMA_PROVIDER_H

#include <infiniband/verbs.h>

#include <cstdint>
#include <deque>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "transport/provider.h"

namespace hux {

struct RdmaConfig {
  std::string device_name;   /* Empty selects the first port that is up. */
  uint8_t ib_port = 1;
  /* Queue pairs per connection. More of them raises the number of requests in
   * flight, not the number of network paths -- the two are often confused.
   * One stays supported as the baseline every measurement compares against. */
  uint32_t qp_per_conn = 1;
  /* Signal one work request in every N. Completions are the only way posted
   * entries are reclaimed, so a period that leaves none signalled would fill
   * the queue and stall it permanently. */
  uint32_t signal_period = 16;
  int gid_index = -1;        /* Negative asks for automatic selection. */
  uint32_t cq_depth = 4096;
  uint32_t sq_depth = 1024;
  /* Receives exist only to catch peers' arrival signals. The queue has to be
   * deep enough that a burst of writes does not exhaust it between polls. */
  uint32_t rq_depth = 64;
  uint32_t max_sge = 1;
  uint16_t listen_port = 0;  /* 0 lets the kernel choose. */
  /* Address peers should dial back on. A provider cannot pick this itself on
   * a multi-homed host, so the caller states it. */
  std::string advertise_ip = "127.0.0.1";
};

/* Wire format of the connection handshake. A major mismatch is refused rather
 * than tolerated: the two ends would otherwise agree to a layout only one of
 * them understands, and the damage surfaces as corrupt transfers rather than
 * a failed connect. */
/* Bumped to 2 when the handshake started carrying a queue pair count: the
 * layout changed, so an older peer must be refused rather than left to
 * misread it. */
constexpr uint16_t kWireMajor = 2;
constexpr uint16_t kWireMinor = 0;

/* Identifies a request's sub-operation, so a completion can name what it
 * belongs to. */
struct InflightKey {
  RequestId request = 0;
  uint64_t sub_id = 0;
  bool is_write = false;
};

/* Identifies one end of a queue pair. Exchanged over TCP during connect, in a
 * fixed little-endian layout rather than as a raw struct. */
struct RdmaEndpointInfo {
  uint16_t major = kWireMajor;
  uint16_t minor = kWireMinor;
  uint32_t qp_num = 0;
  uint16_t lid = 0;
  uint8_t gid[16] = {};
  uint32_t psn = 0;
  uint32_t mtu = 0;
};

class RdmaConnection;

/* Separate from the exchange so it can be tested without two hosts. */
Status check_wire_version(uint16_t peer_major, uint16_t peer_minor);

class RdmaProvider : public TransportProvider {
 public:
  /* Opens the device and starts the bootstrap listener. Returns a specific
   * error rather than a null object, so a caller never holds a provider that
   * looks alive but cannot post. */
  static Status create(RdmaConfig const& cfg,
                       std::shared_ptr<RdmaProvider>* out);

  ~RdmaProvider() override;

  ProviderCaps caps() const override;
  ProviderStats stats() const override;

  Status register_region(void* addr, uint64_t length, DeviceId device,
                         AccessFlags access, uint64_t* local_key,
                         uint64_t* remote_key) override;
  Status deregister_region(uint64_t local_key) override;

  Status connect(std::vector<uint8_t> const& peer_metadata,
                 ProviderConnectionPtr* out) override;
  Status disconnect(ProviderConnectionPtr conn) override;
  Status local_metadata(std::vector<uint8_t>* out) const override;

  SubmitResult submit(ProviderConnection* conn,
                      std::vector<SubOp> const& ops) override;
  Status poll(uint32_t max_events, std::vector<CompletionEvent>* out) override;
  Status send_control(ProviderConnection* conn, uint16_t type,
                      std::vector<uint8_t> const& payload) override;
  Status poll_control(uint32_t max_items,
                      std::vector<ControlMessage>* out) override;
  Status poll_peer_arrivals(uint32_t max_items,
                            std::vector<PeerArrival>* out) override;
  Status flush(ProviderConnection* conn) override;
  Status drain(ProviderConnection* conn, int64_t timeout_ms) override;

  /* Blocks until one peer connects, for the passive side of a test or an
   * application that has no out-of-band channel of its own. */
  Status accept(int64_t timeout_ms, ProviderConnectionPtr* out);

  ibv_pd* pd() const { return pd_; }
  ibv_cq* cq() const { return cq_; }
  RdmaConfig const& config() const { return cfg_; }

 private:
  friend class RdmaConnection;

  Status open_device();
  Status start_listener();
  Status build_connection(int sock, ProviderConnectionPtr* out);

  RdmaConfig cfg_;
  ibv_context* ctx_ = nullptr;
  ibv_pd* pd_ = nullptr;
  ibv_cq* cq_ = nullptr;
  ibv_port_attr port_attr_{};
  ibv_gid local_gid_{};
  int gid_index_ = 0;
  int listen_fd_ = -1;
  uint16_t listen_port_ = 0;
  std::string local_ip_;

  mutable std::mutex mu_;
  std::unordered_map<uint64_t, ibv_mr*> regions_;
  uint64_t next_key_ = 1;
  ProviderStats stats_;

  /* Maps a work request id back to the sub-operation that produced it. The
   * CQE only carries wr_id, so without this a failure could not name the
   * request it belongs to. */
  struct InflightOp {
    RequestId request = 0;
    uint64_t sub_id = 0;
    bool is_write = false;
    /* The CQ is shared across connections, so a completion has to name the
     * one whose outstanding count it retires. */
    RdmaConnection* conn = nullptr;
  };
  std::mutex inflight_mu_;
  std::unordered_map<uint64_t, InflightOp> inflight_;
  uint64_t next_wr_id_ = 1;

  std::mutex arrival_mu_;
  std::deque<PeerArrival> arrivals_;

  /* A receive completion names its QP, not its connection, so the two are
   * mapped here to re-arm the right one. Entries are removed by the
   * connection's destructor. */
  std::mutex conn_mu_;
  std::unordered_map<uint32_t, RdmaConnection*> conn_by_qp_;
  std::vector<std::weak_ptr<RdmaConnection>> ctrl_conns_;
  void register_conn(uint32_t qp_num, RdmaConnection* c);
  void forget_conn(uint32_t qp_num);
};

/* Per queue pair accounting. Each one tracks its own sequence numbers and its
 * own signalling anchor: a global counter deciding which work request to
 * signal, while the queue pairs rotate, can leave one of them with no anchor
 * at all and no way to reclaim its queue. */
struct QueuePair {
  ibv_qp* qp = nullptr;
  uint64_t posted = 0;      /* sequence number of the next work request */
  uint64_t reclaimed = 0;   /* everything below this has completed */
  uint32_t since_signal = 0;
  /* Sub-operations posted without a signal, waiting for the next signalled
   * completion to retire them. Within one queue pair completion order follows
   * posting order, so a signalled completion retires everything before it. */
  std::deque<std::pair<uint64_t, InflightKey>> unsignalled;
};

class RdmaConnection : public ProviderConnection {
 public:
  RdmaConnection(RdmaProvider* owner, std::vector<QueuePair> qps,
                 std::vector<RdmaEndpointInfo> remote);
  ~RdmaConnection();

  uint32_t qp_count() const override {
    return static_cast<uint32_t>(qps_.size());
  }
  uint32_t submit_capacity() const override;

  /* Picks the queue pair with the most room. Round robin is kept as the
   * comparison point rather than the default, since a rotation that ignores
   * depth piles onto one that is already full. */
  /* Caller holds qp_mutex(). Kept explicit so submission and completion
   * cannot interleave halfway through updating a queue pair's counters. */
  QueuePair* pick_queue_pair_locked();
  std::mutex& qp_mutex() { return qp_mu_; }
  std::vector<QueuePair>& queue_pairs() { return qps_; }
  ibv_qp* qp() const { return qps_.empty() ? nullptr : qps_[0].qp; }

  /* Receive work requests exist only to catch the immediate value a peer
   * sends with its final write; no payload lands in them. One has to be
   * posted before the peer writes, or the arrival is lost. */
  Status arm_receives(ibv_pd* pd, uint32_t count);
  Status repost_receive();

  /* An RC queue pair that hits a fatal completion moves to ERROR and flushes
   * everything after it. Without marking that, later requests fail one by one
   * with no indication the connection itself is gone. */
  bool failed() const { return failed_.load(std::memory_order_acquire); }
  void mark_failed() { failed_.store(true, std::memory_order_release); }

 private:
  RdmaProvider* owner_;
  std::vector<QueuePair> qps_;
  std::vector<RdmaEndpointInfo> remote_;
  mutable std::mutex qp_mu_;
  uint32_t next_qp_ = 0;
  std::atomic<bool> failed_{false};
  ibv_mr* recv_mr_ = nullptr;
  std::vector<uint8_t> recv_buf_;

 public:
  /* The bootstrap socket is kept as the control channel rather than closed
   * after the handshake. It is independent of the RDMA path, so control
   * traffic keeps moving when the data path is congested or broken. */
  int ctrl_fd = -1;
  std::mutex send_mu_;      /* serializes header and payload together */
  std::vector<uint8_t> rx;  /* partial message carried between polls */
};

}  // namespace hux
#endif  // HUX_TRANSPORT_RDMA_PROVIDER_H
