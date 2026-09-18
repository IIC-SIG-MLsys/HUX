/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "transport/rdma/rdma_provider.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace hux {
namespace {

void put_u16(std::vector<uint8_t>* o, uint16_t v) {
  o->push_back(v & 0xff);
  o->push_back((v >> 8) & 0xff);
}
void put_u32(std::vector<uint8_t>* o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o->push_back((v >> (8 * i)) & 0xff);
}
uint16_t get_u16(uint8_t const* p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t get_u32(uint8_t const* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i);
  return v;
}

bool send_all(int fd, void const* buf, size_t n) {
  auto* p = static_cast<uint8_t const*>(buf);
  while (n > 0) {
    ssize_t k = ::send(fd, p, n, 0);
    if (k <= 0) return false;
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool recv_all(int fd, void* buf, size_t n) {
  auto* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    ssize_t k = ::recv(fd, p, n, 0);
    if (k <= 0) return false;
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

constexpr size_t kEpInfoBytes = 4 + 2 + 16 + 4 + 4;

void encode_ep(RdmaEndpointInfo const& e, uint8_t* out) {
  std::vector<uint8_t> b;
  put_u32(&b, e.qp_num);
  put_u16(&b, e.lid);
  b.insert(b.end(), e.gid, e.gid + 16);
  put_u32(&b, e.psn);
  put_u32(&b, e.mtu);
  std::memcpy(out, b.data(), kEpInfoBytes);
}

void decode_ep(uint8_t const* in, RdmaEndpointInfo* e) {
  e->qp_num = get_u32(in);
  e->lid = get_u16(in + 4);
  std::memcpy(e->gid, in + 6, 16);
  e->psn = get_u32(in + 22);
  e->mtu = get_u32(in + 26);
}

/* Maps a completion status onto the contract. IBV_WC_REM_ACCESS_ERR and its
 * neighbours mean the remote side may already have been written, which the
 * caller has to know before deciding what the target holds. */
Status status_from_wc(ibv_wc_status s, bool* may_have_modified) {
  *may_have_modified = false;
  switch (s) {
    case IBV_WC_SUCCESS:
      return Status::kOk;
    case IBV_WC_RETRY_EXC_ERR:
    case IBV_WC_RNR_RETRY_EXC_ERR:
      return Status::kPeerDisconnected;
    case IBV_WC_REM_ACCESS_ERR:
    case IBV_WC_REM_OP_ERR:
    case IBV_WC_REM_INV_REQ_ERR:
      *may_have_modified = true;
      return Status::kTransportError;
    case IBV_WC_WR_FLUSH_ERR:
      /* Queued behind a failure that already aborted the QP. */
      return Status::kCancelled;
    case IBV_WC_LOC_PROT_ERR:
    case IBV_WC_LOC_LEN_ERR:
      return Status::kInvalidArgument;
    default:
      *may_have_modified = true;
      return Status::kTransportError;
  }
}

int ib_access_flags(AccessFlags a) {
  int f = IBV_ACCESS_LOCAL_WRITE;
  if (has_flag(a, AccessFlags::kRemoteRead)) f |= IBV_ACCESS_REMOTE_READ;
  if (has_flag(a, AccessFlags::kRemoteWrite)) f |= IBV_ACCESS_REMOTE_WRITE;
  return f;
}

}  // namespace

// ---------------- RdmaConnection ----------------

RdmaConnection::RdmaConnection(RdmaProvider* owner, ibv_qp* qp,
                               RdmaEndpointInfo remote)
    : owner_(owner), qp_(qp), remote_(remote) {}

RdmaConnection::~RdmaConnection() {
  if (qp_ != nullptr) {
    owner_->forget_conn(qp_->qp_num);
    ibv_destroy_qp(qp_);
  }
  if (recv_mr_ != nullptr) ibv_dereg_mr(recv_mr_);
}

/* kRecvWrId marks a completion as belonging to the receive queue; the send
 * side never uses it, so the two cannot be confused when the CQ is shared. */
namespace {
constexpr uint64_t kRecvWrId = (1ull << 63);
}

Status RdmaConnection::arm_receives(ibv_pd* pd, uint32_t count) {
  recv_buf_.assign(count * 16, 0);
  recv_mr_ = ibv_reg_mr(pd, recv_buf_.data(), recv_buf_.size(),
                        IBV_ACCESS_LOCAL_WRITE);
  if (recv_mr_ == nullptr) return Status::kDeviceError;
  for (uint32_t i = 0; i < count; ++i) {
    Status s = repost_receive();
    if (s != Status::kOk) return s;
  }
  return Status::kOk;
}

Status RdmaConnection::repost_receive() {
  if (recv_mr_ == nullptr) return Status::kUnsupported;
  ibv_sge sge{};
  sge.addr = reinterpret_cast<uint64_t>(recv_buf_.data());
  sge.length = 16;
  sge.lkey = recv_mr_->lkey;

  ibv_recv_wr wr{};
  wr.wr_id = kRecvWrId;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  ibv_recv_wr* bad = nullptr;
  return ibv_post_recv(qp_, &wr, &bad) == 0 ? Status::kOk
                                            : Status::kTransportError;
}

uint32_t RdmaConnection::submit_capacity() const {
  uint32_t depth = owner_->config().sq_depth;
  uint32_t used = outstanding_.load(std::memory_order_acquire);
  return used >= depth ? 0 : depth - used;
}

void RdmaConnection::note_posted(uint32_t n) {
  outstanding_.fetch_add(n, std::memory_order_acq_rel);
}

void RdmaConnection::note_completed(uint32_t n) {
  /* Never let the counter wrap: a completion for a work request this
   * connection did not post would otherwise inflate its capacity forever. */
  uint32_t cur = outstanding_.load(std::memory_order_acquire);
  while (true) {
    uint32_t dec = n < cur ? n : cur;
    if (dec == 0) return;
    if (outstanding_.compare_exchange_weak(cur, cur - dec,
                                           std::memory_order_acq_rel))
      return;
  }
}

// ---------------- RdmaProvider ----------------

void RdmaProvider::register_conn(uint32_t qp_num, RdmaConnection* c) {
  std::lock_guard<std::mutex> g(conn_mu_);
  conn_by_qp_[qp_num] = c;
}

void RdmaProvider::forget_conn(uint32_t qp_num) {
  std::lock_guard<std::mutex> g(conn_mu_);
  conn_by_qp_.erase(qp_num);
}

Status RdmaProvider::create(RdmaConfig const& cfg,
                            std::shared_ptr<RdmaProvider>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  auto p = std::shared_ptr<RdmaProvider>(new RdmaProvider());
  p->cfg_ = cfg;
  Status s = p->open_device();
  if (s != Status::kOk) return s;
  s = p->start_listener();
  if (s != Status::kOk) return s;
  *out = std::move(p);
  return Status::kOk;
}

RdmaProvider::~RdmaProvider() {
  if (listen_fd_ >= 0) ::close(listen_fd_);
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& kv : regions_) ibv_dereg_mr(kv.second);
    regions_.clear();
  }
  if (cq_ != nullptr) ibv_destroy_cq(cq_);
  if (pd_ != nullptr) ibv_dealloc_pd(pd_);
  if (ctx_ != nullptr) ibv_close_device(ctx_);
}

Status RdmaProvider::open_device() {
  int num = 0;
  ibv_device** list = ibv_get_device_list(&num);
  if (list == nullptr || num == 0) return Status::kDeviceError;

  ibv_device* chosen = nullptr;
  for (int i = 0; i < num; ++i) {
    char const* name = ibv_get_device_name(list[i]);
    if (!cfg_.device_name.empty()) {
      if (cfg_.device_name == name) {
        chosen = list[i];
        break;
      }
      continue;
    }
    /* No name given: take the first port that is actually up, rather than
     * device zero, which may well be down. */
    ibv_context* c = ibv_open_device(list[i]);
    if (c == nullptr) continue;
    ibv_port_attr pa{};
    if (ibv_query_port(c, cfg_.ib_port, &pa) == 0 &&
        pa.state == IBV_PORT_ACTIVE) {
      chosen = list[i];
      ibv_close_device(c);
      break;
    }
    ibv_close_device(c);
  }
  if (chosen == nullptr) {
    ibv_free_device_list(list);
    return Status::kDeviceError;
  }

  ctx_ = ibv_open_device(chosen);
  ibv_free_device_list(list);
  if (ctx_ == nullptr) return Status::kDeviceError;

  if (ibv_query_port(ctx_, cfg_.ib_port, &port_attr_) != 0)
    return Status::kDeviceError;

  /* RoCE needs a GID index and the two ends need not agree on which one, so
   * it is resolved locally and carried in the metadata. */
  gid_index_ = cfg_.gid_index;
  if (gid_index_ < 0) {
    gid_index_ = port_attr_.link_layer == IBV_LINK_LAYER_ETHERNET ? 3 : 0;
  }
  if (ibv_query_gid(ctx_, cfg_.ib_port, gid_index_, &local_gid_) != 0)
    return Status::kDeviceError;

  pd_ = ibv_alloc_pd(ctx_);
  if (pd_ == nullptr) return Status::kDeviceError;
  cq_ = ibv_create_cq(ctx_, static_cast<int>(cfg_.cq_depth), nullptr, nullptr, 0);
  if (cq_ == nullptr) return Status::kDeviceError;
  return Status::kOk;
}

Status RdmaProvider::start_listener() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return Status::kInternal;
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(cfg_.listen_port);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    return Status::kInternal;
  if (::listen(listen_fd_, 8) != 0) return Status::kInternal;

  socklen_t len = sizeof(addr);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
    return Status::kInternal;
  listen_port_ = ntohs(addr.sin_port);
  return Status::kOk;
}

ProviderCaps RdmaProvider::caps() const {
  ProviderCaps c;
  c.name = "rdma";
  c.supports_read = true;
  c.supports_write = true;
  c.supports_vector = false;  /* Core splits into scalar sub-operations. */
  c.supports_multi_qp = false;
  c.needs_explicit_flush = false;
  c.supports_peer_signal = true;
  c.max_segment_bytes = 0;
  c.max_sge = cfg_.max_sge;
  return c;
}

ProviderStats RdmaProvider::stats() const {
  std::lock_guard<std::mutex> g(mu_);
  return stats_;
}

Status RdmaProvider::register_region(void* addr, uint64_t length, DeviceId,
                                     AccessFlags access, uint64_t* local_key,
                                     uint64_t* remote_key) {
  if (addr == nullptr || length == 0 || local_key == nullptr ||
      remote_key == nullptr)
    return Status::kInvalidArgument;

  ibv_mr* mr = ibv_reg_mr(pd_, addr, static_cast<size_t>(length),
                          ib_access_flags(access));
  if (mr == nullptr) {
    /* ENOMEM here is a registration quota, not host memory pressure, and the
     * two call for different responses from the caller. */
    return errno == ENOMEM ? Status::kResourceExhausted : Status::kDeviceError;
  }

  std::lock_guard<std::mutex> g(mu_);
  uint64_t key = next_key_++;
  regions_[key] = mr;
  *local_key = key;
  *remote_key = mr->rkey;
  return Status::kOk;
}

Status RdmaProvider::deregister_region(uint64_t local_key) {
  ibv_mr* mr = nullptr;
  {
    std::lock_guard<std::mutex> g(mu_);
    auto it = regions_.find(local_key);
    if (it == regions_.end()) return Status::kNotFound;
    mr = it->second;
    regions_.erase(it);
  }
  return ibv_dereg_mr(mr) == 0 ? Status::kOk : Status::kDeviceError;
}

Status RdmaProvider::local_metadata(std::vector<uint8_t>* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  put_u16(out, listen_port_);
  put_u16(out, static_cast<uint16_t>(gid_index_));
  put_u16(out, static_cast<uint16_t>(cfg_.advertise_ip.size()));
  out->insert(out->end(), cfg_.advertise_ip.begin(), cfg_.advertise_ip.end());
  return Status::kOk;
}

/* Brings a fresh QP through RESET -> INIT -> RTR -> RTS. Each transition is
 * checked: a QP left in the wrong state fails later at post time, where the
 * cause is far less obvious. */
Status RdmaProvider::build_connection(int sock, ProviderConnectionPtr* out) {
  ibv_qp_init_attr init{};
  init.send_cq = cq_;
  init.recv_cq = cq_;
  init.qp_type = IBV_QPT_RC;
  init.cap.max_send_wr = cfg_.sq_depth;
  init.cap.max_recv_wr = cfg_.rq_depth;
  init.cap.max_send_sge = cfg_.max_sge;
  init.cap.max_recv_sge = 1;

  ibv_qp* qp = ibv_create_qp(pd_, &init);
  if (qp == nullptr) return Status::kDeviceError;

  RdmaEndpointInfo mine;
  mine.qp_num = qp->qp_num;
  mine.lid = port_attr_.lid;
  std::memcpy(mine.gid, &local_gid_, 16);
  mine.psn = 0;
  mine.mtu = static_cast<uint32_t>(port_attr_.active_mtu);

  /* Exchange before any transition, so both ends know the peer's QP number
   * and MTU. */
  uint8_t tx[kEpInfoBytes], rx[kEpInfoBytes];
  encode_ep(mine, tx);
  if (!send_all(sock, tx, kEpInfoBytes) || !recv_all(sock, rx, kEpInfoBytes)) {
    ibv_destroy_qp(qp);
    return Status::kPeerDisconnected;
  }
  RdmaEndpointInfo peer;
  decode_ep(rx, &peer);

  ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = cfg_.ib_port;
  attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                         IBV_ACCESS_REMOTE_WRITE;
  if (ibv_modify_qp(qp, &attr,
                    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                        IBV_QP_ACCESS_FLAGS) != 0) {
    ibv_destroy_qp(qp);
    return Status::kDeviceError;
  }

  std::memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  /* Take the smaller of the two MTUs. Path MTU is not negotiated by the
   * hardware, and a mismatch shows up much later as a length error on the
   * first transfer that exceeds the smaller side. */
  attr.path_mtu = static_cast<ibv_mtu>(
      std::min<uint32_t>(mine.mtu, peer.mtu));
  attr.dest_qp_num = peer.qp_num;
  attr.rq_psn = mine.psn;
  attr.max_dest_rd_atomic = 16;
  attr.min_rnr_timer = 12;
  attr.ah_attr.port_num = cfg_.ib_port;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  if (port_attr_.link_layer == IBV_LINK_LAYER_ETHERNET) {
    attr.ah_attr.is_global = 1;
    std::memcpy(&attr.ah_attr.grh.dgid, peer.gid, 16);
    attr.ah_attr.grh.sgid_index = static_cast<uint8_t>(gid_index_);
    attr.ah_attr.grh.hop_limit = 255;
  } else {
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = peer.lid;
  }
  if (ibv_modify_qp(qp, &attr,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) {
    ibv_destroy_qp(qp);
    return Status::kDeviceError;
  }

  std::memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.sq_psn = peer.psn;
  attr.max_rd_atomic = 16;
  if (ibv_modify_qp(qp, &attr,
                    IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                        IBV_QP_MAX_QP_RD_ATOMIC) != 0) {
    ibv_destroy_qp(qp);
    return Status::kDeviceError;
  }

  auto conn = std::make_shared<RdmaConnection>(this, qp, peer);
  register_conn(qp->qp_num, conn.get());
  /* Armed before the connection is handed out, so a peer writing immediately
   * afterwards still finds a receive waiting. */
  Status rs = conn->arm_receives(pd_, cfg_.rq_depth);
  if (rs != Status::kOk) return rs;
  *out = conn;
  return Status::kOk;
}

Status RdmaProvider::connect(std::vector<uint8_t> const& peer_metadata,
                             ProviderConnectionPtr* out) {
  if (out == nullptr || peer_metadata.size() < 6) return Status::kInvalidArgument;
  uint16_t port = get_u16(peer_metadata.data());
  uint16_t ip_len = get_u16(peer_metadata.data() + 4);
  if (peer_metadata.size() != 6u + ip_len) return Status::kInvalidArgument;
  std::string ip(reinterpret_cast<char const*>(peer_metadata.data() + 6), ip_len);

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return Status::kInternal;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    ::close(sock);
    return Status::kInvalidArgument;
  }
  if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(sock);
    return Status::kPeerDisconnected;
  }
  Status s = build_connection(sock, out);
  ::close(sock);
  return s;
}

Status RdmaProvider::accept(int64_t timeout_ms, ProviderConnectionPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
  while (true) {
    timeval tv{0, 200000};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_fd_, &rfds);
    int r = ::select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv);
    if (r > 0) {
      int sock = ::accept(listen_fd_, nullptr, nullptr);
      if (sock < 0) continue;
      Status s = build_connection(sock, out);
      ::close(sock);
      return s;
    }
    if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline)
      return Status::kTimeout;
  }
}

/* Posts as many operations as the queue will take, in order, and reports how
 * far it got. A refusal partway through is not an error for the operations
 * already accepted: those are in flight and will produce completions, so the
 * caller must keep their resources and roll back only the remainder. */
SubmitResult RdmaProvider::submit(ProviderConnection* conn,
                                  std::vector<SubOp> const& ops) {
  SubmitResult r;
  if (conn == nullptr) {
    r.status = Status::kInvalidArgument;
    return r;
  }
  auto* c = static_cast<RdmaConnection*>(conn);
  if (c->failed()) {
    /* Posting onto a queue pair in ERROR only produces flush completions. Say
     * so plainly instead of letting each request fail on its own. */
    r.status = Status::kPeerDisconnected;
    return r;
  }

  uint32_t const room = c->submit_capacity();
  if (room == 0) {
    r.status = Status::kWouldBlock;
    return r;
  }

  for (size_t i = 0; i < ops.size(); ++i) {
    if (i >= room) {
      /* Out of queue space rather than broken: the caller retries the rest. */
      r.status = Status::kWouldBlock;
      break;
    }
    SubOp const& op = ops[i];

    ibv_mr* mr = nullptr;
    {
      std::lock_guard<std::mutex> g(mu_);
      auto it = regions_.find(op.local_key);
      if (it == regions_.end()) {
        r.status = Status::kNotFound;
        break;
      }
      mr = it->second;
    }

    uint64_t wr_id;
    {
      std::lock_guard<std::mutex> g(inflight_mu_);
      wr_id = next_wr_id_++;
      inflight_[wr_id] = {op.request, op.sub_id,
                          op.kind == SubOp::Kind::kWrite, c};
    }

    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(op.local_addr);
    sge.length = static_cast<uint32_t>(op.length);
    sge.lkey = mr->lkey;

    ibv_send_wr wr{};
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    if (op.kind == SubOp::Kind::kWrite) {
      /* The last sub-operation of a write carries an immediate value, which
       * consumes a receive on the peer and so becomes visible to its CPU.
       * Ordinary writes before it stay invisible, which is the point: one
       * signal per logical request rather than per chunk. */
      wr.opcode = op.signal_peer ? IBV_WR_RDMA_WRITE_WITH_IMM
                                 : IBV_WR_RDMA_WRITE;
      if (op.signal_peer) wr.imm_data = htonl(op.peer_token);
    } else {
      wr.opcode = IBV_WR_RDMA_READ;
    }
    /* Every work request is signalled for now. Selective signalling belongs
     * with the multi-QP work, where the recovery anchors have to be reasoned
     * about properly rather than bolted on. */
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = op.remote_addr;
    wr.wr.rdma.rkey = static_cast<uint32_t>(op.remote_key);

    ibv_send_wr* bad = nullptr;
    int rc = ibv_post_send(c->qp(), &wr, &bad);
    if (rc != 0) {
      {
        std::lock_guard<std::mutex> g(inflight_mu_);
        inflight_.erase(wr_id);
      }
      r.status = rc == ENOMEM ? Status::kWouldBlock : Status::kTransportError;
      r.provider_errno = rc;
      break;
    }
    ++r.accepted;
    {
      std::lock_guard<std::mutex> g(mu_);
      ++stats_.subops_posted;
      stats_.payload_bytes += op.length;
      /* Nothing is added to payload_bytes_copied: the work request points at
       * the caller's own memory, so the NIC reads and writes it directly. */
    }
  }

  c->note_posted(r.accepted);
  return r;
}

/* Drains up to max_events completions and hands back every one of them.
 * Stopping early on a particular request would lose the completions sharing
 * the batch, and those requests would never reach a terminal state. */
Status RdmaProvider::poll(uint32_t max_events, std::vector<CompletionEvent>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  if (max_events == 0) return Status::kOk;

  std::vector<ibv_wc> wc(max_events);
  int n = ibv_poll_cq(cq_, static_cast<int>(max_events), wc.data());
  if (n < 0) return Status::kTransportError;

  for (int i = 0; i < n; ++i) {
    if ((wc[i].wr_id & kRecvWrId) != 0) {
      /* A peer's write landed. The immediate value names the handoff; the
       * receive buffer itself holds nothing. */
      if (wc[i].status == IBV_WC_SUCCESS &&
          wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        std::lock_guard<std::mutex> g(arrival_mu_);
        arrivals_.push_back(PeerArrival{ntohl(wc[i].imm_data), 0});
      }
      /* Re-arm regardless: a consumed receive that is not replaced silently
       * lowers the number of arrivals that can still be caught. */
      {
        std::lock_guard<std::mutex> g(conn_mu_);
        auto it = conn_by_qp_.find(wc[i].qp_num);
        if (it != conn_by_qp_.end()) it->second->repost_receive();
      }
      continue;
    }
    InflightOp op;
    bool known = false;
    {
      std::lock_guard<std::mutex> g(inflight_mu_);
      auto it = inflight_.find(wc[i].wr_id);
      if (it != inflight_.end()) {
        op = it->second;
        known = true;
        inflight_.erase(it);
      }
    }
    if (!known) continue;  /* Already reported, or not ours. */
    if (op.conn != nullptr) op.conn->note_completed(1);

    CompletionEvent ev;
    ev.request = op.request;
    ev.sub_id = op.sub_id;
    bool modified = false;
    ev.status = status_from_wc(wc[i].status, &modified);
    /* Anything but a flush means this completion is what broke the queue
     * pair; a flush is the wreckage of an earlier one. Either way the
     * connection is unusable from here. */
    if (wc[i].status != IBV_WC_SUCCESS && op.conn != nullptr)
      op.conn->mark_failed();
    ev.provider_errno = static_cast<int32_t>(wc[i].status);
    /* Only a write can have changed the remote side. */
    ev.may_have_modified_target = modified && op.is_write;
    {
      std::lock_guard<std::mutex> g(mu_);
      if (ev.status == Status::kOk) ++stats_.subops_completed;
      else ++stats_.subops_failed;
    }
    out->push_back(ev);
  }
  return Status::kOk;
}

Status RdmaProvider::poll_peer_arrivals(uint32_t max_items,
                                       std::vector<PeerArrival>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  std::lock_guard<std::mutex> g(arrival_mu_);
  while (!arrivals_.empty() && out->size() < max_items) {
    out->push_back(arrivals_.front());
    arrivals_.pop_front();
  }
  return Status::kOk;
}

Status RdmaProvider::flush(ProviderConnection*) {
  /* RDMA completions already imply the write reached the remote HCA; no
   * separate flush step, unlike UCX. */
  return Status::kOk;
}

Status RdmaProvider::drain(ProviderConnection* conn, int64_t timeout_ms) {
  if (conn == nullptr) return Status::kInvalidArgument;
  auto* c = static_cast<RdmaConnection*>(conn);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
  std::vector<CompletionEvent> evs;
  while (c->submit_capacity() < cfg_.sq_depth) {
    poll(cfg_.cq_depth, &evs);  /* retires the counts itself */
    if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) {
      /* Reporting a timeout rather than returning ok matters: the caller must
       * not free memory that may still be under DMA. */
      return Status::kTimeout;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return Status::kOk;
}

Status RdmaProvider::disconnect(ProviderConnectionPtr conn) {
  /* The connection owns its QP and destroys it; in-flight work requests are
   * flushed by the hardware and surface as WR_FLUSH_ERR completions, which
   * poll() still reports so their requests can reach a terminal state. */
  (void)conn;
  return Status::kOk;
}

}  // namespace hux
