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
  int gid_index = -1;        /* Negative asks for automatic selection. */
  uint32_t cq_depth = 4096;
  uint32_t sq_depth = 1024;
  uint32_t max_sge = 1;
  uint16_t listen_port = 0;  /* 0 lets the kernel choose. */
  /* Address peers should dial back on. A provider cannot pick this itself on
   * a multi-homed host, so the caller states it. */
  std::string advertise_ip = "127.0.0.1";
};

/* Identifies one end of a queue pair. Exchanged over TCP during connect, in a
 * fixed little-endian layout rather than as a raw struct. */
struct RdmaEndpointInfo {
  uint32_t qp_num = 0;
  uint16_t lid = 0;
  uint8_t gid[16] = {};
  uint32_t psn = 0;
  uint32_t mtu = 0;
};

class RdmaConnection;

class RdmaProvider : public TransportProvider {
 public:
  /* Opens the device and starts the bootstrap listener. Returns a specific
   * error rather than a null object, so a caller never holds a provider that
   * looks alive but cannot post. */
  static Status create(RdmaConfig const& cfg,
                       std::shared_ptr<RdmaProvider>* out);

  ~RdmaProvider() override;

  ProviderCaps caps() const override;

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
};

class RdmaConnection : public ProviderConnection {
 public:
  RdmaConnection(RdmaProvider* owner, ibv_qp* qp, RdmaEndpointInfo remote);
  ~RdmaConnection();

  uint32_t qp_count() const override { return 1; }
  uint32_t submit_capacity() const override;

  ibv_qp* qp() const { return qp_; }
  void note_posted(uint32_t n);
  void note_completed(uint32_t n);

 private:
  RdmaProvider* owner_;
  ibv_qp* qp_ = nullptr;
  RdmaEndpointInfo remote_;
  std::atomic<uint32_t> outstanding_{0};
};

}  // namespace hux
#endif  // HUX_TRANSPORT_RDMA_PROVIDER_H
