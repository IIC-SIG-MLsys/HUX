/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "transport/rdma/rdma_provider.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>

#include "control/control_message.h"
#include "transport/rdma/mlx5_ordering.h"

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

/* Writes all of the handshake, or says it could not.
 *
 * Only the handshake. Waiting for room is the right thing while two
 * processes are agreeing on endpoints -- the caller is inside connect() and
 * expects to wait -- and the wrong thing afterwards, when control messages
 * come from the completion path and a wait there stops every transfer on
 * the engine. Those go through ControlOutbox, which never waits.
 *
 * MSG_NOSIGNAL, because a peer that has closed its end turns a write into
 * SIGPIPE, and nothing here installs a handler: the process would die of a
 * peer disconnecting. The IPC provider passes it; this one did not.
 *
 * And EAGAIN is not a disconnect. The socket carries a send timeout during
 * the handshake, so a peer that is slow to answer -- a process holding a
 * transport per adapter, answering one handshake at a time -- came back as
 * the peer having gone away. It waits for room instead, bounded. */
bool send_all(int fd, void const* buf, size_t n, int timeout_ms = 30000) {
  auto* p = static_cast<uint8_t const*>(buf);
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (n > 0) {
    ssize_t k = ::send(fd, p, n, MSG_NOSIGNAL);
    if (k > 0) {
      p += k;
      n -= static_cast<size_t>(k);
      continue;
    }
    if (k < 0 && errno == EINTR) continue;
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      if (left <= 0) return false;
      pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLOUT;
      if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) return false;
      continue;
    }
    return false;
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

/* A RoCE v2 IPv4 GID is an IPv4-mapped address: ten zero bytes, 0xffff, then
 * the four address bytes. */
bool gid_ipv4(ibv_gid const& g, std::string* out) {
  for (int i = 0; i < 10; ++i)
    if (g.raw[i] != 0) return false;
  if (g.raw[10] != 0xff || g.raw[11] != 0xff) return false;
  in_addr a{};
  std::memcpy(&a, g.raw + 12, 4);
  char buf[INET_ADDRSTRLEN];
  if (::inet_ntop(AF_INET, &a, buf, sizeof(buf)) == nullptr) return false;
  *out = buf;
  return true;
}

/* Verbs does not report the GID type, so it is read where the kernel
 * publishes it. Only v2 carries an IP header, so only v2 is routed. */
bool gid_is_roce_v2(char const* device, uint8_t port, int index) {
  std::string path = std::string("/sys/class/infiniband/") + device +
                     "/ports/" + std::to_string(port) + "/gid_attrs/types/" +
                     std::to_string(index);
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  char buf[64] = {0};
  ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  return n > 0 && std::string(buf).find("v2") != std::string::npos;
}

/* Scans a port's GID table. With an address, returns the index carrying it;
 * without one, the first routable entry. -1 when the port has neither. */
int find_roce_v2_gid(ibv_context* c, char const* device, uint8_t port,
                     int table_len, std::string const& ip) {
  int first = -1;
  for (int i = 0; i < table_len; ++i) {
    ibv_gid g{};
    if (ibv_query_gid(c, port, i, &g) != 0) continue;
    std::string addr;
    if (!gid_ipv4(g, &addr)) continue;
    if (!gid_is_roce_v2(device, port, i)) continue;
    if (ip.empty()) return i;
    if (addr == ip) return i;
    if (first < 0) first = i;
  }
  return ip.empty() ? first : -1;
}

/* Whether an address is one a peer on another host could dial. Loopback and
 * the wildcard name no port, so they carry no constraint. */
bool routable_ipv4(std::string const& ip) {
  in_addr a{};
  if (ip.empty() || ::inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
  uint32_t h = ntohl(a.s_addr);
  return h != 0 && (h >> 24) != 127;
}

constexpr size_t kEpInfoBytes = 2 + 2 + 4 + 2 + 16 + 4 + 4;

void encode_ep(RdmaEndpointInfo const& e, uint8_t* out) {
  std::vector<uint8_t> b;
  put_u16(&b, e.major);
  put_u16(&b, e.minor);
  put_u32(&b, e.qp_num);
  put_u16(&b, e.lid);
  b.insert(b.end(), e.gid, e.gid + 16);
  put_u32(&b, e.psn);
  put_u32(&b, e.mtu);
  std::memcpy(out, b.data(), kEpInfoBytes);
}

void decode_ep(uint8_t const* in, RdmaEndpointInfo* e) {
  e->major = get_u16(in);
  e->minor = get_u16(in + 2);
  e->qp_num = get_u32(in + 4);
  e->lid = get_u16(in + 8);
  std::memcpy(e->gid, in + 10, 16);
  e->psn = get_u32(in + 26);
  e->mtu = get_u32(in + 30);
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

Status check_wire_version(uint16_t peer_major, uint16_t peer_minor) {
  /* A newer minor on the peer is fine: minor changes only add fields this end
   * can ignore. A different major is not, in either direction. */
  if (peer_major != kWireMajor) return Status::kUnsupported;
  (void)peer_minor;
  return Status::kOk;
}

// ---------------- RdmaConnection ----------------

RdmaConnection::RdmaConnection(RdmaProvider* owner, std::vector<QueuePair> qps,
                               std::vector<RdmaEndpointInfo> remote)
    : owner_(owner), qps_(std::move(qps)), remote_(std::move(remote)) {}

RdmaConnection::~RdmaConnection() {
  if (ctrl_fd >= 0) ::close(ctrl_fd);
  for (auto& q : qps_) {
    if (q.qp != nullptr) {
      owner_->forget_conn(q.qp->qp_num);
      ibv_destroy_qp(q.qp);
    }
  }
  if (recv_mr_ != nullptr) ibv_dereg_mr(recv_mr_);
}

/* kRecvWrId marks a completion as belonging to the receive queue; the send
 * side never uses it, so the two cannot be confused when the CQ is shared. */
namespace {
constexpr uint64_t kRecvWrId = (1ull << 63);
/* And one the data path never mints either, so a connection proving itself
 * cannot be mistaken for a transfer. */
constexpr uint64_t kVerifyWrId = (1ull << 62);
}  // namespace

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
  /* Receives live on the first queue pair only: they exist to catch arrival
   * signals, and a peer sends one per logical request, not per queue pair. */
  if (recv_mr_ == nullptr || qps_.empty()) return Status::kUnsupported;
  ibv_sge sge{};
  sge.addr = reinterpret_cast<uint64_t>(recv_buf_.data());
  sge.length = 16;
  sge.lkey = recv_mr_->lkey;

  ibv_recv_wr wr{};
  wr.wr_id = kRecvWrId;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  ibv_recv_wr* bad = nullptr;
  return ibv_post_recv(qps_[0].qp, &wr, &bad) == 0 ? Status::kOk
                                                   : Status::kTransportError;
}

Status RdmaConnection::verify_path(int64_t timeout_ms) {
  if (qps_.empty() || recv_mr_ == nullptr) return Status::kUnsupported;
  ibv_qp* const qp = qps_[0].qp;
  uint32_t const my_qp_num = qp->qp_num;

  ibv_sge sge{};
  sge.addr = reinterpret_cast<uint64_t>(recv_buf_.data());
  sge.length = 8;
  sge.lkey = recv_mr_->lkey;

  ibv_send_wr wr{};
  wr.wr_id = kVerifyWrId;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;
  ibv_send_wr* bad = nullptr;
  if (ibv_post_send(qp, &wr, &bad) != 0) return Status::kTransportError;

  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    /* Left here by a progress thread that drew it from the queue first. */
    ibv_wc_status st;
    if (owner_->take_verified(my_qp_num, &st))
      return st == IBV_WC_SUCCESS ? Status::kOk : Status::kTransportError;

    ibv_wc wc[8];
    int const n = ibv_poll_cq(owner_->cq(), 8, wc);
    if (n < 0) return Status::kTransportError;
    for (int i = 0; i < n; ++i) {
      if (wc[i].wr_id == kVerifyWrId && wc[i].qp_num == my_qp_num) {
        /* Success means the far end acknowledged it. Anything else means
         * this path does not work, whatever its state says. */
        return wc[i].status == IBV_WC_SUCCESS ? Status::kOk
                                              : Status::kTransportError;
      }
      if ((wc[i].wr_id & kRecvWrId) != 0 && wc[i].qp_num == my_qp_num) {
        /* The peer's own verification landing here. Consume it and put the
         * receive back, or the queue is one short for ever after. */
        repost_receive();
        continue;
      }
      /* Someone else's transfer. Drawn from a queue shared with every other
       * connection, so it goes back to the engine rather than being lost. */
      owner_->stash_completion(wc[i]);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return Status::kTimeout;
}

uint32_t RdmaConnection::submit_capacity() const {
  uint32_t const depth = owner_->config().sq_depth;
  uint64_t room = 0;
  std::lock_guard<std::mutex> g(qp_mu_);
  for (auto const& q : qps_) {
    uint64_t used = q.posted - q.reclaimed;
    if (used < depth) room += depth - used;
  }
  return static_cast<uint32_t>(room);
}

QueuePair* RdmaConnection::pick_queue_pair_locked() {
  uint32_t const depth = owner_->config().sq_depth;
  QueuePair* best = nullptr;
  uint64_t best_room = 0;
  size_t const n = qps_.size();
  for (size_t i = 0; i < n; ++i) {
    /* Starts from a rotating offset so equal queues are still spread, then
     * prefers depth: a plain rotation keeps handing work to a queue that is
     * already full while others sit idle. */
    QueuePair& q = qps_[(next_qp_ + i) % n];
    uint64_t used = q.posted - q.reclaimed;
    uint64_t room = used >= depth ? 0 : depth - used;
    if (room > best_room) {
      best_room = room;
      best = &q;
    }
  }
  next_qp_ = (next_qp_ + 1) % static_cast<uint32_t>(n);
  return best;
}

// ---------------- RdmaProvider ----------------

void RdmaProvider::forget_conn_of(RdmaConnection* c) {
  std::lock_guard<std::mutex> g(conn_mu_);
  for (auto it = conn_by_qp_.begin(); it != conn_by_qp_.end();)
    it = it->second == c ? conn_by_qp_.erase(it) : std::next(it);
  for (auto it = ctrl_conns_.begin(); it != ctrl_conns_.end();) {
    auto held = it->lock();
    /* Expired entries go too: this is the only place that walks the list. */
    it = (held == nullptr || held.get() == c) ? ctrl_conns_.erase(it)
                                              : std::next(it);
  }
}

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
  p->cc_ = cfg.cc != nullptr ? cfg.cc : make_cc_off();
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

  /* An explicit name wins; otherwise the nearest NIC to the device, if one
   * was given; otherwise the first port that is up. */
  std::string wanted = cfg_.device_name;
  int ip_gid = -1;
  /* An address peers dial back on is a reachability constraint, not a
   * preference: only the port that carries it can be reached. It therefore
   * outranks affinity below, which is a performance choice. */
  if (wanted.empty() && routable_ipv4(cfg_.advertise_ip)) {
    for (int i = 0; i < num; ++i) {
      char const* name = ibv_get_device_name(list[i]);
      ibv_context* c = ibv_open_device(list[i]);
      if (c == nullptr) continue;
      ibv_port_attr pa{};
      if (ibv_query_port(c, cfg_.ib_port, &pa) == 0 &&
          pa.state == IBV_PORT_ACTIVE) {
        int g = find_roce_v2_gid(c, name, cfg_.ib_port, pa.gid_tbl_len,
                                 cfg_.advertise_ip);
        if (g >= 0) {
          wanted = name;
          ip_gid = g;
        }
      }
      ibv_close_device(c);
      if (ip_gid >= 0) break;
    }
  }
  if (wanted.empty() && cfg_.has_affinity) {
    NicInfo nic;
    Proximity how = Proximity::kUnknown;
    if (best_nic_for(cfg_.affinity, discover_nics(), &nic, &how)) {
      wanted = nic.name;
      nic_proximity_ = how;
    }
    /* No pairing established: fall through to the first active port rather
     * than naming one on a guess. */
  }

  ibv_device* chosen = nullptr;
  for (int i = 0; i < num; ++i) {
    char const* name = ibv_get_device_name(list[i]);
    if (!wanted.empty()) {
      if (wanted == name) {
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

  bool commands = false;
  char const* no_commands = "";
  ctx_ = mlx5_open_device(chosen, &commands, &no_commands);
  ibv_free_device_list(list);
  if (ctx_ == nullptr) return Status::kDeviceError;

  if (ibv_query_port(ctx_, cfg_.ib_port, &port_attr_) != 0)
    return Status::kDeviceError;

  /* RoCE needs a GID index and the two ends need not agree on which one, so
   * it is resolved locally and carried in the metadata. */
  gid_index_ = cfg_.gid_index;
  if (gid_index_ < 0) gid_index_ = ip_gid;
  if (gid_index_ < 0 && port_attr_.link_layer == IBV_LINK_LAYER_ETHERNET) {
    /* Index 3 is a common layout, not a rule -- one host here puts its
     * routable entry at 5 -- so the table is scanned rather than assumed. */
    gid_index_ =
        find_roce_v2_gid(ctx_, ibv_get_device_name(ctx_->device), cfg_.ib_port,
                         port_attr_.gid_tbl_len, std::string());
  }
  if (gid_index_ < 0) {
    gid_index_ = port_attr_.link_layer == IBV_LINK_LAYER_ETHERNET ? 3 : 0;
  }
  if (ibv_query_gid(ctx_, cfg_.ib_port, gid_index_, &local_gid_) != 0)
    return Status::kDeviceError;

  /* Out-of-order placement is offered where this end can do it; whether a
   * connection uses it is settled with each peer. RoCE v2 over IPv4 only:
   * the address path it is brought up with is the Ethernet one, its hop
   * limit is taken the way the kernel takes it for IPv4, and nothing here
   * has run over an InfiniBand fabric. */
  std::string v4;
  if (!cfg_.out_of_order) {
    ooo_why_ = "off";
  } else if (!commands) {
    ooo_why_ = no_commands;
  } else if (port_attr_.link_layer != IBV_LINK_LAYER_ETHERNET ||
             !gid_is_roce_v2(ibv_get_device_name(ctx_->device), cfg_.ib_port,
                             gid_index_) ||
             !gid_ipv4(local_gid_, &v4)) {
    ooo_why_ = "not RoCE v2 over IPv4";
  } else {
    ooo_rw_ = mlx5_ooo_rw_supported(ctx_, &log_max_msg_, &ooo_why_) &&
              mlx5_kernel_path(ibv_get_device_name(ctx_->device), cfg_.ib_port,
                               &ooo_hop_limit_, &ooo_why_);
  }

  pd_ = ibv_alloc_pd(ctx_);
  if (pd_ == nullptr) return Status::kDeviceError;
  cq_ =
      ibv_create_cq(ctx_, static_cast<int>(cfg_.cq_depth), nullptr, nullptr, 0);
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
  c.name = cfg_.nic_ordinal == 0 ? std::string("rdma")
                                 : "rdma#" + std::to_string(cfg_.nic_ordinal);
  c.supports_read = true;
  c.supports_write = true;
  c.supports_vector = false; /* Core splits into scalar sub-operations. */
  c.supports_multi_qp = false;
  c.needs_explicit_flush = false;
  /* False deliberately: an immediate value rides one queue pair, and one
   * queue pair completing says nothing about the others. Arrival is announced
   * over the control channel once every sub-operation is done. */
  c.supports_peer_signal = false;
  c.relative_capacity = cfg_.relative_capacity;
  c.max_segment_bytes = 0;
  c.max_sge = cfg_.max_sge;
  return c;
}

size_t RdmaProvider::queued_control_messages() const {
  size_t n = 0;
  std::lock_guard<std::mutex> g(conn_mu_);
  for (auto const& w : ctrl_conns_) {
    auto c = w.lock();
    if (c != nullptr) n += c->outbox.backlog_messages();
  }
  return n;
}

size_t RdmaProvider::out_of_order_connections() const {
  size_t n = 0;
  std::lock_guard<std::mutex> g(conn_mu_);
  for (auto const& w : ctrl_conns_) {
    auto c = w.lock();
    if (c != nullptr && c->ooo_rw) ++n;
  }
  return n;
}

std::string RdmaProvider::describe() const {
  std::ostringstream o;
  o << "{"
    << "\"provider\":\"rdma\","
    << "\"advertise_ip\":\"" << cfg_.advertise_ip << "\","
    << "\"device\":\""
    << (ctx_ != nullptr ? ibv_get_device_name(ctx_->device) : "none") << "\","
    << "\"ib_port\":" << static_cast<int>(cfg_.ib_port) << ','
    << "\"nic_proximity\":\"" << to_string(nic_proximity_) << "\","
    << "\"gid_index\":" << gid_index_ << ',' << "\"link_layer\":\""
    << (port_attr_.link_layer == IBV_LINK_LAYER_ETHERNET ? "ethernet"
                                                         : "infiniband")
    << "\","
    << "\"active_mtu\":" << static_cast<int>(port_attr_.active_mtu) << ','
    << "\"qp_per_conn\":" << cfg_.qp_per_conn << ','
    << "\"sq_depth\":" << cfg_.sq_depth << ','
    << "\"rq_depth\":" << cfg_.rq_depth << ','
    << "\"cq_depth\":" << cfg_.cq_depth << ','
    << "\"signal_period\":" << cfg_.signal_period
    << ','
    /* Offered by this end, and how many live connections have it: both
     * ends have to offer it for a connection to use it. */
    << "\"out_of_order\":\"" << (ooo_rw_ ? "offered" : ooo_why_) << "\","
    << "\"out_of_order_connections\":" << out_of_order_connections()
    << ','
    /* Control messages written but not yet taken by the peer. Non-zero at
     * the end of a run means the peer was not reading its control channel
     * and these never arrived: the transfers succeeded and nothing failed,
     * so this is the only sign. */
    << "\"ctrl_queued\":" << queued_control_messages() << ',' << "\"cc\":\""
    << (cc_ != nullptr ? cc_->name() : "none") << "\","
    << "\"cc_window_bytes\":"
    << (cc_ != nullptr ? cc_->window_bytes(CcDirection::kWrite) : 0)
    << ','
    /* What the controller settled on, not what it was configured with. A
     * rate-based controller that ramps too slowly to reach line rate within
     * a run looks like a slow transport unless this is visible. Zero for a
     * controller that does not work in rates. */
    << "\"cc_rate_bps\":"
    << (cc_ != nullptr ? cc_->rate_bytes_per_sec(CcDirection::kWrite) : 0.0)
    << "}";
  return o.str();
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

/* What every queue pair is brought up with, through verbs or through the
 * adapter's commands alike, so the two ways cannot drift apart. */
namespace {
constexpr uint8_t kRdAtomic = 16;
constexpr uint8_t kMinRnrTimer = 12;
constexpr uint8_t kAckTimeout = 14;
constexpr uint8_t kRetryCount = 7;
constexpr uint8_t kRnrRetry = 7;
}  // namespace

/* Brings a fresh QP through RESET -> INIT -> RTR -> RTS. Each transition is
 * checked: a QP left in the wrong state fails later at post time, where the
 * cause is far less obvious. */
Status RdmaProvider::build_connection(int sock, ProviderConnectionPtr* out) {
  uint32_t const n = cfg_.qp_per_conn == 0 ? 1 : cfg_.qp_per_conn;

  std::vector<QueuePair> qps(n);
  for (uint32_t i = 0; i < n; ++i) {
    ibv_qp_init_attr init{};
    init.send_cq = cq_;
    init.recv_cq = cq_;
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = cfg_.sq_depth;
    /* Only the first queue pair receives; the others never post one. */
    init.cap.max_recv_wr = i == 0 ? cfg_.rq_depth : 1;
    init.cap.max_send_sge = cfg_.max_sge;
    init.cap.max_recv_sge = 1;
    qps[i].qp = ibv_create_qp(pd_, &init);
    if (qps[i].qp == nullptr) {
      for (auto& q : qps)
        if (q.qp != nullptr) ibv_destroy_qp(q.qp);
      return Status::kDeviceError;
    }
  }

  auto destroy_all = [&] {
    for (auto& q : qps)
      if (q.qp != nullptr) ibv_destroy_qp(q.qp);
  };

  /* The count goes first so the peer knows how much follows. Both ends must
   * agree on it: pairing N against M would leave queue pairs connected to
   * nothing. */
  std::vector<uint8_t> tx(4 + n * kEpInfoBytes);
  tx[0] = static_cast<uint8_t>(n & 0xff);
  tx[1] = static_cast<uint8_t>((n >> 8) & 0xff);
  tx[2] = static_cast<uint8_t>((n >> 16) & 0xff);
  tx[3] = static_cast<uint8_t>((n >> 24) & 0xff);
  for (uint32_t i = 0; i < n; ++i) {
    RdmaEndpointInfo mine;
    mine.qp_num = qps[i].qp->qp_num;
    mine.lid = port_attr_.lid;
    std::memcpy(mine.gid, &local_gid_, 16);
    mine.psn = 0;
    mine.mtu = static_cast<uint32_t>(port_attr_.active_mtu);
    encode_ep(mine, tx.data() + 4 + i * kEpInfoBytes);
  }

  if (!send_all(sock, tx.data(), tx.size())) {
    destroy_all();
    return Status::kPeerDisconnected;
  }

  uint8_t count_buf[4];
  if (!recv_all(sock, count_buf, 4)) {
    destroy_all();
    return Status::kPeerDisconnected;
  }
  uint32_t peer_n = static_cast<uint32_t>(count_buf[0]) |
                    (static_cast<uint32_t>(count_buf[1]) << 8) |
                    (static_cast<uint32_t>(count_buf[2]) << 16) |
                    (static_cast<uint32_t>(count_buf[3]) << 24);
  if (peer_n != n) {
    /* Refused rather than trimmed to the smaller side: a caller that asked
     * for a given width should hear that it did not get it. */
    destroy_all();
    return Status::kInvalidArgument;
  }

  std::vector<uint8_t> rx(n * kEpInfoBytes);
  if (!recv_all(sock, rx.data(), rx.size())) {
    destroy_all();
    return Status::kPeerDisconnected;
  }

  std::vector<RdmaEndpointInfo> peers(n);
  for (uint32_t i = 0; i < n; ++i)
    decode_ep(rx.data() + i * kEpInfoBytes, &peers[i]);

  Status vs = check_wire_version(peers[0].major, peers[0].minor);
  if (vs != Status::kOk) {
    /* Refused before any queue pair transition, so neither end is left with a
     * half-built connection. */
    destroy_all();
    return vs;
  }

  /* Features, with a peer that exchanges them. Both ends decide from the
   * same two words, so they cannot come out differently. */
  uint32_t peer_features = 0;
  if (peers[0].minor >= 1) {
    std::vector<uint8_t> mine;
    put_u32(&mine, ooo_rw_ ? kFeatureOooRw : 0u);
    uint8_t theirs[4];
    if (!send_all(sock, mine.data(), mine.size()) ||
        !recv_all(sock, theirs, sizeof(theirs))) {
      destroy_all();
      return Status::kPeerDisconnected;
    }
    peer_features = get_u32(theirs);
  }
  bool const ooo = ooo_rw_ && (peer_features & kFeatureOooRw) != 0;

  /* Each queue pair is taken through RESET -> INIT -> RTR -> RTS against its
   * own peer. One left in the wrong state fails later at post time, where the
   * cause is much harder to see. */
  for (uint32_t i = 0; i < n; ++i) {
    ibv_qp* qp = qps[i].qp;
    RdmaEndpointInfo const& peer = peers[i];
    uint32_t const my_mtu = static_cast<uint32_t>(port_attr_.active_mtu);

    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = cfg_.ib_port;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS) != 0) {
      destroy_all();
      return Status::kDeviceError;
    }

    if (ooo) {
      /* The same transitions with the same values as below, made by the
       * adapter's own commands so they can carry the ordering. A failure is
       * returned rather than retried through verbs: the peer is bringing up
       * its end out of order, and a connection whose two ends disagree is
       * not one this has been run on. */
      Mlx5Connect m;
      m.remote_qpn = peer.qp_num;
      m.path_mtu = std::min<uint32_t>(my_mtu, peer.mtu);
      m.port = cfg_.ib_port;
      m.sgid_index = static_cast<uint8_t>(gid_index_);
      m.hop_limit = ooo_hop_limit_;
      std::memcpy(m.dgid, peer.gid, 16);
      m.max_dest_rd_atomic = kRdAtomic;
      m.max_rd_atomic = kRdAtomic;
      m.min_rnr_timer = kMinRnrTimer;
      m.timeout = kAckTimeout;
      m.retry_cnt = kRetryCount;
      m.rnr_retry = kRnrRetry;
      m.log_max_msg = log_max_msg_;
      Status const ms = mlx5_connect_ooo_rw(qp, m);
      if (ms != Status::kOk) {
        destroy_all();
        return ms;
      }
      continue;
    }

    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    /* The smaller of the two MTUs. Path MTU is not negotiated in hardware,
     * and a mismatch surfaces much later as a length error on the first
     * transfer that exceeds the smaller side. */
    attr.path_mtu = static_cast<ibv_mtu>(std::min<uint32_t>(my_mtu, peer.mtu));
    attr.dest_qp_num = peer.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = kRdAtomic;
    attr.min_rnr_timer = kMinRnrTimer;
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
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) !=
        0) {
      destroy_all();
      return Status::kDeviceError;
    }

    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = kAckTimeout;
    attr.retry_cnt = kRetryCount;
    attr.rnr_retry = kRnrRetry;
    attr.sq_psn = 0;
    attr.max_rd_atomic = kRdAtomic;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                          IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                          IBV_QP_MAX_QP_RD_ATOMIC) != 0) {
      destroy_all();
      return Status::kDeviceError;
    }
  }

  auto conn =
      std::make_shared<RdmaConnection>(this, std::move(qps), std::move(peers));
  conn->ooo_rw = ooo;
  /* Hand the socket to the connection instead of closing it; it becomes the
   * control channel. Non-blocking so polling never stalls progress. */
  int flags = ::fcntl(sock, F_GETFL, 0);
  ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);
  int nodelay = 1;
  ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  conn->ctrl_fd = sock;
  conn->outbox.reset(sock);
  for (auto& q : conn->queue_pairs()) register_conn(q.qp->qp_num, conn.get());
  {
    std::lock_guard<std::mutex> g(conn_mu_);
    ctrl_conns_.push_back(conn);
  }
  /* Armed before the connection is handed out, so a peer writing immediately
   * afterwards still finds a receive waiting. */
  Status rs = conn->arm_receives(pd_, cfg_.rq_depth);
  if (rs != Status::kOk) return rs;

  /* And proved, before it is handed out. Reaching ready is not evidence that
   * anything can cross: two addresses on one subnet bring both queue pairs
   * up while the kernel route sends every reply out the other interface, and
   * the caller is given a lane that will never complete a transfer. With one
   * lane that is a peer that does not work; with several it is worse, since
   * a request needs every lane's share and one silent lane hangs the rest. */
  Status const proved = conn->verify_path(cfg_.verify_timeout_ms);
  if (proved != Status::kOk) {
    forget_conn_of(conn.get());
    return proved;
  }
  *out = conn;
  return Status::kOk;
}

Status RdmaProvider::connect(std::vector<uint8_t> const& peer_metadata,
                             ProviderConnectionPtr* out) {
  if (out == nullptr || peer_metadata.size() < 6)
    return Status::kInvalidArgument;
  uint16_t port = get_u16(peer_metadata.data());
  uint16_t ip_len = get_u16(peer_metadata.data() + 4);
  if (peer_metadata.size() != 6u + ip_len) return Status::kInvalidArgument;
  std::string ip(reinterpret_cast<char const*>(peer_metadata.data() + 6),
                 ip_len);

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
  /* Bounded. The peer's listener accepting the socket says nothing about
   * anything being ready to exchange endpoints on it -- a process holding a
   * transport per adapter and accepting on one of them at a time will leave
   * the others' handshakes unanswered for as long as it likes. Blocking
   * there would hang the caller inside what it thinks is a connect, and with
   * several lanes it would hang the whole peer over one adapter. */
  timeval tv{30, 0};
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  Status s = build_connection(sock, out);
  /* Blocking again once it is up: the control channel is polled, not waited
   * on, and a receive timeout there would look like the peer going away. */
  if (s == Status::kOk) {
    timeval none{0, 0};
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));
  }
  /* Not closed: build_connection keeps it as the control channel. On failure
   * it never took ownership, so it is closed here. */
  if (s != Status::kOk) ::close(sock);
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
      if (s != Status::kOk) ::close(sock);
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
/* Work request ids encode which queue pair posted them and the sequence
 * number within it, so a completion can be traced back without a lookup
 * table. The top bit marks a receive. */
namespace {
constexpr uint64_t kQpShift = 48;
inline uint64_t make_wr_id(uint32_t qp_index, uint64_t seq) {
  return (static_cast<uint64_t>(qp_index) << kQpShift) |
         (seq & ((1ull << kQpShift) - 1));
}
inline uint32_t wr_qp_index(uint64_t id) {
  return static_cast<uint32_t>((id >> kQpShift) & 0x7fff);
}
inline uint64_t wr_seq(uint64_t id) { return id & ((1ull << kQpShift) - 1); }
}  // namespace

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
  if (ops.empty()) return r;
  if (c->submit_capacity() == 0) {
    r.status = Status::kWouldBlock;
    return r;
  }

  std::lock_guard<std::mutex> qg(c->qp_mutex());
  auto& qps = c->queue_pairs();
  uint32_t const depth = cfg_.sq_depth;

  /* Assignment happens first, posting second.
   *
   * Signalling has to be decided with the whole batch in view. Marking only
   * the last operation of the batch leaves every other queue pair it touched
   * without an anchor: those work requests produce no completion, are never
   * reclaimed, and their requests never finish. Each queue pair used needs
   * its own last operation signalled. */
  struct Planned {
    uint32_t qp_index = 0;
    size_t op_index = 0;
    bool signal = false;
  };
  std::vector<Planned> plan;
  plan.reserve(ops.size());
  std::vector<uint64_t> next_seq(qps.size());
  std::vector<uint32_t> since(qps.size());
  for (size_t i = 0; i < qps.size(); ++i) {
    next_seq[i] = qps[i].posted;
    since[i] = qps[i].since_signal;
  }
  /* Index into plan of the last operation on each queue pair, or npos. */
  std::vector<size_t> last_on_qp(qps.size(), static_cast<size_t>(-1));

  for (size_t i = 0; i < ops.size(); ++i) {
    QueuePair* q = c->pick_queue_pair_locked();
    if (q == nullptr) {
      r.status = Status::kWouldBlock;
      break;
    }
    uint32_t const qi = static_cast<uint32_t>(q - qps.data());
    uint64_t const used = next_seq[qi] - qps[qi].reclaimed;
    if (used >= depth) {
      r.status = Status::kWouldBlock;
      break;
    }

    /* Budget is claimed here, while planning, not at post time. Checking
     * without claiming would let every operation in one batch see the same
     * empty window and all be admitted -- the limit would exist and never
     * bind. Anything planned but not posted is released below. */
    CcDirection const dir = ops[i].kind == SubOp::Kind::kWrite
                                ? CcDirection::kWrite
                                : CcDirection::kRead;
    CcTime const plan_time = CcClock::now();
    if (cc_->allow(dir, ops[i].length, plan_time) != CcVerdict::kAllowed) {
      r.status = Status::kWouldBlock;
      break;
    }
    cc_->on_post(dir, ops[i].length, plan_time);

    Planned p;
    p.qp_index = qi;
    p.op_index = i;
    /* Period elapsed, or the queue is three quarters full: both leave an
     * anchor before the queue can fill with nothing to wait for. */
    p.signal = (since[qi] + 1 >= cfg_.signal_period) ||
               (used + 1 >= depth - depth / 4);
    if (p.signal)
      since[qi] = 0;
    else
      ++since[qi];
    ++next_seq[qi];
    last_on_qp[qi] = plan.size();
    plan.push_back(p);
  }

  /* Every queue pair this batch touched ends with a signalled operation. */
  for (size_t qi = 0; qi < last_on_qp.size(); ++qi) {
    if (last_on_qp[qi] != static_cast<size_t>(-1))
      plan[last_on_qp[qi]].signal = true;
  }

  size_t posted_count = 0;
  for (auto const& p : plan) {
    SubOp const& op = ops[p.op_index];
    QueuePair& q = qps[p.qp_index];

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

    uint64_t const seq = q.posted;

    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(op.local_addr);
    sge.length = static_cast<uint32_t>(op.length);
    sge.lkey = mr->lkey;

    ibv_send_wr wr{};
    wr.wr_id = make_wr_id(p.qp_index, seq);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode =
        op.kind == SubOp::Kind::kWrite ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
    wr.send_flags = p.signal ? IBV_SEND_SIGNALED : 0;
    wr.wr.rdma.remote_addr = op.remote_addr;
    wr.wr.rdma.rkey = static_cast<uint32_t>(op.remote_key);

    ibv_send_wr* bad = nullptr;
    int rc = ibv_post_send(q.qp, &wr, &bad);
    if (rc != 0) {
      r.status = rc == ENOMEM ? Status::kWouldBlock : Status::kTransportError;
      r.provider_errno = rc;
      break;
    }
    ++posted_count;

    /* Already claimed during planning; only the timestamp is taken here, so
     * latency is measured from the actual post. */
    CcTime const posted_at = CcClock::now();

    q.unsignalled.push_back(
        {seq, InflightKey{op.request, op.sub_id, op.kind == SubOp::Kind::kWrite,
                          op.length, posted_at}});
    ++q.posted;
    q.since_signal = p.signal ? 0 : q.since_signal + 1;

    ++r.accepted;
    {
      std::lock_guard<std::mutex> g(mu_);
      ++stats_.subops_posted;
      stats_.payload_bytes += op.length;
      /* Nothing is added to payload_bytes_copied: the work request points at
       * the caller's own memory, so the NIC reads and writes it directly. */
    }
  }

  /* Whatever was planned but never posted keeps no budget: leaving it claimed
   * would shrink the window a little on every partial submit until nothing
   * could be sent at all. */
  for (size_t i = posted_count; i < plan.size(); ++i) {
    SubOp const& op = ops[plan[i].op_index];
    cc_->on_error(op.kind == SubOp::Kind::kWrite ? CcDirection::kWrite
                                                 : CcDirection::kRead,
                  op.length, CcClock::now());
  }
  return r;
}

/* Drains up to max_events completions and hands back every one of them.
 * Stopping early on a particular request would lose the completions sharing
 * the batch, and those requests would never reach a terminal state. */
void RdmaProvider::stash_completion(ibv_wc const& wc) {
  std::lock_guard<std::mutex> g(stash_mu_);
  stashed_.push_back(wc);
}

void RdmaProvider::note_verified(uint32_t qp_num, ibv_wc_status st) {
  std::lock_guard<std::mutex> g(verify_mu_);
  verify_done_[qp_num] = st;
}

bool RdmaProvider::take_verified(uint32_t qp_num, ibv_wc_status* st) {
  std::lock_guard<std::mutex> g(verify_mu_);
  auto it = verify_done_.find(qp_num);
  if (it == verify_done_.end()) return false;
  *st = it->second;
  verify_done_.erase(it);
  return true;
}

Status RdmaProvider::poll(uint32_t max_events,
                          std::vector<CompletionEvent>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  if (max_events == 0) return Status::kOk;

  std::vector<ibv_wc> wc(max_events);
  int n = 0;
  /* Anything set aside while a connection was being verified comes first:
   * it was drawn from the queue before this call and would otherwise never
   * be reported. */
  {
    std::lock_guard<std::mutex> g(stash_mu_);
    while (!stashed_.empty() && n < static_cast<int>(max_events)) {
      wc[n++] = stashed_.front();
      stashed_.pop_front();
    }
  }
  if (n < static_cast<int>(max_events)) {
    int const drawn =
        ibv_poll_cq(cq_, static_cast<int>(max_events) - n, wc.data() + n);
    if (drawn < 0) return Status::kTransportError;
    n += drawn;
  }

  for (int i = 0; i < n; ++i) {
    if (wc[i].wr_id == kVerifyWrId) {
      /* A connection proving itself. This loop and that one both read the
       * same queue, so whichever gets there first leaves it for the other
       * rather than reporting it as a transfer that nothing asked for. */
      note_verified(wc[i].qp_num, wc[i].status);
      continue;
    }
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

    RdmaConnection* c = nullptr;
    {
      std::lock_guard<std::mutex> g(conn_mu_);
      auto it = conn_by_qp_.find(wc[i].qp_num);
      if (it != conn_by_qp_.end()) c = it->second;
    }
    if (c == nullptr) continue;

    bool modified = false;
    Status st = status_from_wc(wc[i].status, &modified);
    /* Anything but a flush means this completion is what broke the queue
     * pair; a flush is the wreckage of an earlier one. Either way the
     * connection is unusable from here. */
    if (wc[i].status != IBV_WC_SUCCESS) c->mark_failed();

    uint32_t const qp_index = wr_qp_index(wc[i].wr_id);
    uint64_t const seq = wr_seq(wc[i].wr_id);

    /* Within one queue pair, completions follow posting order, so a signalled
     * completion retires every unsignalled work request before it. Reclaiming
     * per queue pair is what keeps the accounting honest: a global counter
     * would let one queue pair's progress mask another's stall. */
    std::lock_guard<std::mutex> qg(c->qp_mutex());
    auto& qps = c->queue_pairs();
    if (qp_index >= qps.size()) continue;
    QueuePair& q = qps[qp_index];

    CcTime const completed_at = CcClock::now();
    while (!q.unsignalled.empty() && q.unsignalled.front().first <= seq) {
      InflightKey const key = q.unsignalled.front().second;
      q.unsignalled.pop_front();

      CcDirection const dir =
          key.is_write ? CcDirection::kWrite : CcDirection::kRead;
      if (st == Status::kOk) {
        cc_->on_feedback(dir, key.bytes,
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             completed_at - key.posted_at),
                         completed_at);
      } else {
        /* Released on failure too, and exactly once either way: a failure
         * that leaked window would shrink the effective limit every time. */
        cc_->on_error(dir, key.bytes, completed_at);
      }

      CompletionEvent ev;
      ev.request = key.request;
      ev.sub_id = key.sub_id;
      ev.bytes = key.bytes;
      ev.status = st;
      ev.provider_errno = static_cast<int32_t>(wc[i].status);
      /* Only a write can have changed the remote side. */
      ev.may_have_modified_target = modified && key.is_write;
      {
        std::lock_guard<std::mutex> g(mu_);
        if (ev.status == Status::kOk)
          ++stats_.subops_completed;
        else
          ++stats_.subops_failed;
      }
      out->push_back(ev);
    }
    q.reclaimed = seq + 1;
  }
  return Status::kOk;
}

Status RdmaProvider::send_control(ProviderConnection* conn, uint16_t type,
                                  std::vector<uint8_t> const& payload) {
  if (conn == nullptr) return Status::kInvalidArgument;
  if (payload.size() > kControlMaxPayload) return Status::kInvalidArgument;
  auto* c = static_cast<RdmaConnection*>(conn);
  if (c->ctrl_fd < 0) return Status::kPeerDisconnected;

  ControlHeader h;
  h.type = static_cast<ControlType>(type);
  h.payload_len = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> msg(kControlHeaderBytes + payload.size());
  encode_control_header(h, msg.data());
  if (!payload.empty())
    std::memcpy(msg.data() + kControlHeaderBytes, payload.data(),
                payload.size());

  /* One buffer, so header and payload cannot be separated by another
   * message or by a partial write: either is enough to leave the peer's
   * reader permanently out of step, taking the next header as this one's
   * body, and it would not find out until some length came back absurd. */
  Status const s = c->outbox.post(std::move(msg));
  if (s == Status::kPeerDisconnected) c->mark_peer_closed();
  return s;
}

Status RdmaProvider::poll_control(uint32_t max_items,
                                  std::vector<ControlMessage>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();

  std::vector<std::shared_ptr<RdmaConnection>> conns;
  {
    std::lock_guard<std::mutex> g(conn_mu_);
    for (auto it = ctrl_conns_.begin(); it != ctrl_conns_.end();) {
      auto c = it->lock();
      if (c == nullptr) {
        it = ctrl_conns_.erase(it);
      } else {
        conns.push_back(std::move(c));
        ++it;
      }
    }
  }

  for (auto& c : conns) {
    if (c->ctrl_fd < 0) continue;
    /* Anything the send path could not push out goes now. This loop is what
     * drives progress, so it is the only place a backlog can drain -- a
     * handoff left queued here is one the peer never hears about. */
    if (c->outbox.flush() == Status::kPeerDisconnected) c->mark_peer_closed();
    /* Reads what is available and keeps any partial message for next time: a
     * header split across two reads must not be parsed as if it were whole. */
    uint8_t buf[4096];
    while (true) {
      ssize_t n = ::recv(c->ctrl_fd, buf, sizeof(buf), 0);
      if (n == 0) {
        /* End of file: the peer closed cleanly. Whatever already arrived is
         * still parsed below -- its last message may be the one that
         * explains the departure. */
        c->mark_peer_closed();
        break;
      }
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break; /* nothing more right now */
        /* Anything else ended the connection. A peer killed rather than
         * closed resets it, which arrives as ECONNRESET and not as end of
         * file -- so treating every -1 as "nothing yet" left exactly the
         * departure this loop is here to notice unnoticed. */
        c->mark_peer_closed();
        break;
      }
      c->rx.insert(c->rx.end(), buf, buf + n);
      if (n < static_cast<ssize_t>(sizeof(buf))) break;
    }

    while (c->rx.size() >= kControlHeaderBytes && out->size() < max_items) {
      ControlHeader h;
      if (decode_control_header(c->rx.data(), &h) != Status::kOk) {
        /* A malformed header means the stream cannot be resynchronized;
         * dropping the rest is the only honest option. */
        c->rx.clear();
        break;
      }
      size_t total = kControlHeaderBytes + h.payload_len;
      if (c->rx.size() < total) break; /* wait for the remainder */

      ControlMessage m;
      m.conn = c.get();
      m.type = static_cast<uint16_t>(h.type);
      m.payload.assign(c->rx.begin() + kControlHeaderBytes,
                       c->rx.begin() + total);
      out->push_back(std::move(m));
      c->rx.erase(c->rx.begin(), c->rx.begin() + total);
    }
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
  while (true) {
    /* Drained when no queue pair still holds a posted work request. Comparing
     * total capacity against one queue's depth, as a single-queue version
     * could, would declare a wide connection drained while it was still
     * busy. */
    size_t outstanding = 0;
    {
      std::lock_guard<std::mutex> g(c->qp_mutex());
      for (auto const& q : c->queue_pairs())
        outstanding += q.unsignalled.size();
    }
    if (outstanding == 0) return Status::kOk;

    poll(cfg_.cq_depth, &evs);
    if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) {
      /* Reporting a timeout rather than ok matters: the caller must not free
       * memory that may still be under DMA. */
      return Status::kTimeout;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

Status RdmaProvider::disconnect(ProviderConnectionPtr conn) {
  /* The connection owns its QP and destroys it; in-flight work requests are
   * flushed by the hardware and surface as WR_FLUSH_ERR completions, which
   * poll() still reports so their requests can reach a terminal state. */
  (void)conn;
  return Status::kOk;
}

}  // namespace hux
