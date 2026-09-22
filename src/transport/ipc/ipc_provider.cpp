/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "transport/ipc/ipc_provider.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

namespace hux {
namespace {

/* Frame types on the socket. Control traffic for the engine shares the
 * connection but not the meaning: a region publication is this provider's own
 * business and never reaches poll_control. */
enum : uint16_t {
  kFrameControl = 0,
  kFramePublish = 1,
  kFrameWithdraw = 2,
  kFrameWithdrawAck = 3,
};

void put_u16(std::vector<uint8_t>* o, uint16_t v) {
  o->push_back(v & 0xff);
  o->push_back((v >> 8) & 0xff);
}
void put_u64(std::vector<uint8_t>* o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o->push_back((v >> (8 * i)) & 0xff);
}
uint16_t get_u16(uint8_t const* p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t get_u32(uint8_t const* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i);
  return v;
}
uint64_t get_u64(uint8_t const* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
  return v;
}
void put_u32(std::vector<uint8_t>* o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o->push_back((v >> (8 * i)) & 0xff);
}

/* An interrupted call is not a failure. Both of these run on a socket
 * carrying a five-second timeout -- set for the handshake and deliberately
 * left in place afterwards, since a control frame that cannot go out in that
 * long on a Unix socket means the peer has stopped reading. EAGAIN therefore
 * does mean the deadline passed, and is reported. A signal does not. */
bool send_all(int fd, void const* buf, size_t n) {
  auto const* p = static_cast<uint8_t const*>(buf);
  while (n > 0) {
    ssize_t k = ::send(fd, p, n, MSG_NOSIGNAL);
    if (k > 0) {
      p += k;
      n -= static_cast<size_t>(k);
      continue;
    }
    if (k < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool recv_all(int fd, void* buf, size_t n) {
  auto* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    ssize_t k = ::recv(fd, p, n, 0);
    if (k > 0) {
      p += k;
      n -= static_cast<size_t>(k);
      continue;
    }
    if (k < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

/* Names in the abstract namespace start with a NUL and are not files, so a
 * process that dies leaves nothing behind for the next one to trip over. */
int bind_abstract(std::string const& name) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (name.size() + 1 >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  socklen_t len =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 ||
      ::listen(fd, 8) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int dial_abstract(std::string const& name) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (name.size() + 1 >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  socklen_t len =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

/* Enough of a connection object to carry the contract. There are no queue
 * pairs, but capacity is still bounded: a path that accepts without limit
 * lets a caller build a backlog it cannot see. */
class IpcConnection : public ProviderConnection {
 public:
  explicit IpcConnection(IpcProvider* owner) : owner_(owner) {}
  uint32_t qp_count() const override { return 1; }
  uint32_t submit_capacity() const override { return 1024; }
  bool alive() const override {
    return owner_ != nullptr && owner_->peer_alive();
  }

 private:
  IpcProvider* owner_;
};

}  // namespace

Status IpcProvider::create(IpcConfig const& cfg,
                           std::shared_ptr<DeviceBackend> dev,
                           std::shared_ptr<IpcProvider>* out) {
  if (out == nullptr || dev == nullptr) return Status::kInvalidArgument;

  auto p = std::shared_ptr<IpcProvider>(new IpcProvider());
  p->cfg_ = cfg;
  p->dev_ = std::move(dev);
  p->socket_name_ = cfg.socket_name;
  if (p->socket_name_.empty()) {
    /* Numbered per provider, not per process: two engines in one process
     * would otherwise pick the same name and the second would fail to bind,
     * which reads as a machine with no IPC rather than as a name clash. */
    static std::atomic<uint64_t> seq{0};
    std::ostringstream o;
    o << "hux-ipc-" << local_identity().process << '-'
      << seq.fetch_add(1, std::memory_order_relaxed);
    p->socket_name_ = o.str();
  }
  p->listen_fd_ = bind_abstract(p->socket_name_);
  if (p->listen_fd_ < 0) return Status::kInternal;
  *out = std::move(p);
  return Status::kOk;
}

IpcProvider::~IpcProvider() {
  /* Mappings first: closing them after the socket would leave the peer with
   * no way to learn they are gone. */
  for (auto& kv : imported_) {
    if (kv.second.mapped != nullptr) dev_->close_ipc(kv.second.mapped);
  }
  if (sock_ >= 0) ::close(sock_);
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

ProviderCaps IpcProvider::caps() const {
  ProviderCaps c;
  c.name = "ipc";
  c.supports_read = true;
  c.supports_write = true;
  c.supports_vector = false;
  c.supports_multi_qp = false;
  c.needs_explicit_flush = false; /* The copy has finished when submit does. */
  c.supports_peer_signal = false;
  c.max_segment_bytes = 0;
  c.max_sge = 1;
  return c;
}

ProviderStats IpcProvider::stats() const {
  std::lock_guard<std::mutex> g(mu_);
  return stats_;
}

std::string IpcProvider::describe() const {
  std::lock_guard<std::mutex> g(mu_);
  std::ostringstream o;
  size_t mapped = 0;
  for (auto const& kv : imported_)
    if (kv.second.mapped != nullptr) ++mapped;
  o << "{"
    << "\"provider\":\"ipc\","
    << "\"socket\":\"" << socket_name_ << "\","
    << "\"connected\":" << (sock_ >= 0 && !peer_gone_ ? 1 : 0) << ','
    << "\"peer_gone\":" << (peer_gone_ ? 1 : 0) << ','
    << "\"peer_process\":" << peer_identity_.process << ','
    << "\"exported_regions\":" << exported_.size() << ','
    << "\"imported_regions\":" << imported_.size() << ','
    << "\"mappings_held\":" << mapped << ',';
  /* Addresses, because a mapping that resolved to the wrong allocation looks
   * exactly like a transfer that moved nothing. */
  o << "\"exported\":[";
  bool first = true;
  for (auto const& kv : exported_) {
    if (!first) o << ',';
    first = false;
    o << "{\"key\":" << kv.first
      << ",\"addr\":" << reinterpret_cast<uintptr_t>(kv.second.addr)
      << ",\"length\":" << kv.second.length << '}';
  }
  o << "],\"imported\":[";
  first = true;
  for (auto const& kv : imported_) {
    if (!first) o << ',';
    first = false;
    o << "{\"key\":" << kv.first << ",\"peer_addr\":" << kv.second.peer_addr
      << ",\"mapped\":" << reinterpret_cast<uintptr_t>(kv.second.mapped)
      << ",\"length\":" << kv.second.length << '}';
  }
  o << "]}";
  return o.str();
}

Status IpcProvider::local_metadata(std::vector<uint8_t>* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  /* Just the socket. Identity belongs to the engine's metadata, which
   * carries it ahead of this and strips it before the provider is dialled;
   * repeating it here would leave the provider parsing a copy of a field it
   * does not own. Whether the peer really is on this host is settled in the
   * handshake, where it cannot be taken on a caller's word. */
  out->clear();
  std::lock_guard<std::mutex> g(mu_);
  put_u16(out, static_cast<uint16_t>(socket_name_.size()));
  out->insert(out->end(), socket_name_.begin(), socket_name_.end());
  return Status::kOk;
}

Status IpcProvider::handshake(int fd) {
  /* Bounded. A peer that accepted the connection and then said nothing --
   * or a socket dialled at a name whose owner is not listening on it -- would
   * otherwise leave this blocked in a read for ever, inside what the caller
   * thinks is a connect. */
  timeval tv{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  std::vector<uint8_t> mine;
  put_u16(&mine, kIpcWireMajor);
  put_u16(&mine, kIpcWireMinor);
  encode_identity(local_identity(), &mine);
  if (!send_all(fd, mine.data(), mine.size())) return Status::kPeerDisconnected;

  std::vector<uint8_t> theirs(4 + kIdentityBytes);
  if (!recv_all(fd, theirs.data(), theirs.size()))
    return errno == EAGAIN || errno == EWOULDBLOCK ? Status::kTimeout
                                                   : Status::kPeerDisconnected;
  if (get_u16(theirs.data()) != kIpcWireMajor) return Status::kUnsupported;

  Identity peer;
  if (decode_identity(theirs, 4, &peer) != Status::kOk)
    return Status::kInvalidArgument;
  /* Refused rather than attempted: a handle from another machine names
   * nothing here, and mapping would fail later with an error about the
   * handle rather than about the peer being on the wrong host. */
  if (locality_of(local_identity(), peer) == Locality::kRemote)
    return Status::kUnsupported;
  peer_identity_ = peer;
  return Status::kOk;
}

Status IpcProvider::connect(std::vector<uint8_t> const& peer_metadata,
                            ProviderConnectionPtr* out) {
  if (out == nullptr || peer_metadata.size() < 2)
    return Status::kInvalidArgument;

  uint16_t const n = get_u16(peer_metadata.data());
  if (peer_metadata.size() != 2u + n) return Status::kInvalidArgument;
  std::string name(reinterpret_cast<char const*>(peer_metadata.data() + 2), n);

  int fd = dial_abstract(name);
  if (fd < 0) return Status::kPeerDisconnected;

  std::lock_guard<std::mutex> g(mu_);
  Status s = handshake(fd);
  if (s != Status::kOk) {
    ::close(fd);
    return s;
  }
  sock_ = fd;
  auto conn = std::make_shared<IpcConnection>(this);
  conn_ = conn.get();
  publish_all(fd);
  *out = std::move(conn);
  return Status::kOk;
}

Status IpcProvider::accept(int64_t timeout_ms, ProviderConnectionPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  auto const deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
  while (true) {
    timeval tv{0, 200000};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_fd_, &rfds);
    int r = ::select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv);
    if (r > 0) {
      int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      std::lock_guard<std::mutex> g(mu_);
      Status s = handshake(fd);
      if (s != Status::kOk) {
        ::close(fd);
        return s;
      }
      sock_ = fd;
      auto conn = std::make_shared<IpcConnection>(this);
      conn_ = conn.get();
      publish_all(fd);
      *out = std::move(conn);
      return Status::kOk;
    }
    if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline)
      return Status::kTimeout;
  }
}

Status IpcProvider::disconnect(ProviderConnectionPtr) {
  std::lock_guard<std::mutex> g(mu_);
  /* Mappings go before the socket: once it is closed the peer cannot be told
   * anything, and a mapping left open would keep its allocation pinned with
   * nobody left to release it. */
  for (auto& kv : imported_) {
    if (kv.second.mapped != nullptr) {
      dev_->close_ipc(kv.second.mapped);
      kv.second.mapped = nullptr;
    }
  }
  imported_.clear();
  if (sock_ >= 0) {
    ::close(sock_);
    sock_ = -1;
  }
  conn_ = nullptr;
  return Status::kOk;
}

Status IpcProvider::send_frame(int fd, uint16_t type,
                               std::vector<uint8_t> const& body) {
  if (fd < 0) return Status::kPeerDisconnected;
  std::vector<uint8_t> head;
  put_u32(&head, static_cast<uint32_t>(body.size()));
  put_u16(&head, type);
  if (!send_all(fd, head.data(), head.size())) return Status::kPeerDisconnected;
  if (!body.empty() && !send_all(fd, body.data(), body.size()))
    return Status::kPeerDisconnected;
  return Status::kOk;
}

void IpcProvider::publish_all(int fd) {
  for (auto const& kv : exported_) {
    std::vector<uint8_t> body;
    put_u64(&body, kv.first);
    put_u64(&body, reinterpret_cast<uintptr_t>(kv.second.addr));
    put_u64(&body, kv.second.length);
    put_u64(&body, kv.second.handle.offset);
    put_u64(&body, kv.second.handle.allocation_bytes);
    put_u16(&body, static_cast<uint16_t>(kv.second.handle.bytes.size()));
    body.insert(body.end(), kv.second.handle.bytes.begin(),
                kv.second.handle.bytes.end());
    /* Same as above: a peer that did not hear about a region will refuse
     * every transfer naming it, and the refusal would arrive far from here. */
    if (send_frame(fd, kFramePublish, body) != Status::kOk) {
      peer_is_gone_locked();
      return;
    }
  }
}

Status IpcProvider::register_region(void* addr, uint64_t length, DeviceId,
                                    AccessFlags, uint64_t* local_key,
                                    uint64_t* remote_key) {
  if (addr == nullptr || length == 0 || local_key == nullptr ||
      remote_key == nullptr)
    return Status::kInvalidArgument;

  IpcHandle handle;
  /* Refused here rather than papered over: host memory the caller allocated
   * has no handle another process could map, and a fallback that copied it
   * through a shared buffer would be a different path wearing this one's
   * name. */
  Status s = dev_->export_ipc(addr, length, &handle);
  if (s != Status::kOk) return s;

  std::lock_guard<std::mutex> g(mu_);
  uint64_t const key = next_key_++;
  Exported e;
  e.addr = addr;
  e.length = length;
  e.handle = handle;
  exported_[key] = e;

  if (sock_ >= 0) {
    std::vector<uint8_t> body;
    put_u64(&body, key);
    put_u64(&body, reinterpret_cast<uintptr_t>(addr));
    put_u64(&body, length);
    put_u64(&body, handle.offset);
    put_u64(&body, handle.allocation_bytes);
    put_u16(&body, static_cast<uint16_t>(handle.bytes.size()));
    body.insert(body.end(), handle.bytes.begin(), handle.bytes.end());
    /* A publish the peer never got means it does not know this region
     * exists, and every transfer naming it would fail later with an unknown
     * key -- while this call returned a perfectly good one. The frame only
     * fails on a socket that is not going to carry anything else, so the
     * peer is treated as gone and submissions say so straight away. */
    if (send_frame(sock_, kFramePublish, body) != Status::kOk)
      peer_is_gone_locked();
  }

  /* The two differ, as they do on a NIC: the peer names the region by the key
   * it was published under, never by a handle this side holds. */
  *local_key = key;
  *remote_key = key;
  return Status::kOk;
}

Status IpcProvider::deregister_region(uint64_t local_key) {
  std::unique_lock<std::mutex> g(mu_);
  auto it = exported_.find(local_key);
  if (it == exported_.end()) return Status::kNotFound;
  exported_.erase(it);

  if (sock_ < 0) return Status::kOk;

  std::vector<uint8_t> body;
  put_u64(&body, local_key);
  withdraw_acks_[local_key] = false;
  Status s = send_frame(sock_, kFrameWithdraw, body);
  if (s != Status::kOk) {
    withdraw_acks_.erase(local_key);
    return Status::kOk; /* The peer is gone; its mappings went with it. */
  }

  /* Waited for, not assumed. The peer's mapping outlives this registration,
   * and an allocation freed underneath one leaves the peer reading memory
   * that has been handed to something else. */
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < deadline) {
    int fd = sock_;
    g.unlock();
    pump(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    g.lock();
    auto ack = withdraw_acks_.find(local_key);
    if (ack == withdraw_acks_.end() || ack->second) {
      withdraw_acks_.erase(local_key);
      return Status::kOk;
    }
  }
  withdraw_acks_.erase(local_key);
  /* Said plainly rather than reported as success: the mapping may still be
   * held, so the memory is not yet safe to free. */
  return Status::kTimeout;
}

void IpcProvider::peer_is_gone_locked() {
  if (peer_gone_) return;
  peer_gone_ = true;
  for (auto& kv : imported_) {
    if (kv.second.mapped != nullptr) {
      dev_->close_ipc(kv.second.mapped);
      kv.second.mapped = nullptr;
    }
  }
  imported_.clear();
  if (sock_ >= 0) {
    ::close(sock_);
    sock_ = -1;
  }
}

Status IpcProvider::pump(int fd) {
  if (fd < 0) return Status::kPeerDisconnected;
  bool closed = false;
  for (;;) {
    uint8_t buf[4096];
    ssize_t k = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (k > 0) {
      std::lock_guard<std::mutex> g(mu_);
      inbox_.insert(inbox_.end(), buf, buf + k);
      continue;
    }
    /* Whatever already arrived is still parsed below: the peer's last frame
     * before it went away may be the withdrawal that explains why. */
    if (k == 0) closed = true;
    break; /* EAGAIN, or the peer closed */
  }

  std::lock_guard<std::mutex> g(mu_);
  for (;;) {
    if (inbox_.size() < 6) break;
    uint32_t const len = get_u32(inbox_.data());
    if (inbox_.size() < 6u + len) break;
    uint16_t const type = get_u16(inbox_.data() + 4);
    uint8_t const* body = inbox_.data() + 6;

    if (type == kFrameControl && len >= 2) {
      ControlMessage m;
      m.type = get_u16(body);
      m.payload.assign(body + 2, body + len);
      m.conn = conn_;
      m.peer = peer_identity_.process;
      control_.push_back(std::move(m));
    } else if (type == kFramePublish && len >= 42) {
      Imported im;
      uint64_t const key = get_u64(body);
      im.peer_addr = get_u64(body + 8);
      im.length = get_u64(body + 16);
      im.handle.offset = get_u64(body + 24);
      im.handle.allocation_bytes = get_u64(body + 32);
      uint16_t const hn = get_u16(body + 40);
      if (42u + hn <= len) {
        im.handle.bytes.assign(body + 42, body + 42 + hn);
        /* Recorded, not mapped: a peer may export far more than this side
         * ever reads, and every mapping costs address space and a handle. */
        imported_[key] = im;
      }
    } else if (type == kFrameWithdraw && len >= 8) {
      uint64_t const key = get_u64(body);
      auto it = imported_.find(key);
      if (it != imported_.end()) {
        if (it->second.mapped != nullptr) dev_->close_ipc(it->second.mapped);
        imported_.erase(it);
      }
      std::vector<uint8_t> ack;
      put_u64(&ack, key);
      send_frame(fd, kFrameWithdrawAck, ack);
    } else if (type == kFrameWithdrawAck && len >= 8) {
      uint64_t const key = get_u64(body);
      auto it = withdraw_acks_.find(key);
      if (it != withdraw_acks_.end()) it->second = true;
    }
    inbox_.erase(inbox_.begin(), inbox_.begin() + 6 + len);
  }
  if (closed) {
    peer_is_gone_locked();
    return Status::kPeerDisconnected;
  }
  return Status::kOk;
}

Status IpcProvider::ensure_mapped(uint64_t key, Imported** out) {
  auto it = imported_.find(key);
  if (it == imported_.end()) return Status::kNotFound;
  if (it->second.mapped == nullptr) {
    void* mapped = nullptr;
    Status s = dev_->import_ipc(it->second.handle, &mapped);
    if (s != Status::kOk) return s;
    it->second.mapped = mapped;
  }
  *out = &it->second;
  return Status::kOk;
}

SubmitResult IpcProvider::submit(ProviderConnection* conn,
                                 std::vector<SubOp> const& ops) {
  SubmitResult r;
  if (conn == nullptr) {
    r.status = Status::kInvalidArgument;
    return r;
  }

  /* A publication may still be in the socket when the first operation against
   * it is submitted, so the frames are drained before the keys are looked up
   * rather than after failing to find one. */
  int fd = -1;
  {
    std::lock_guard<std::mutex> g(mu_);
    fd = sock_;
  }
  pump(fd);

  std::lock_guard<std::mutex> g(mu_);
  /* Refused rather than attempted. The mapping may still be addressable
   * after the peer exits, so a copy into it would report success while
   * writing into memory that is no longer the peer's. */
  if (peer_gone_ || sock_ < 0) {
    r.status = Status::kPeerDisconnected;
    return r;
  }

  /* Completions are held back until the batch has settled. A completion
   * means the bytes are in place, and the copies below do not wait
   * individually -- one wait for the batch costs one synchronization rather
   * than one per sub-operation, which was four times the latency here. */
  std::vector<CompletionEvent> landed;
  for (auto const& op : ops) {
    Imported* im = nullptr;
    Status s = ensure_mapped(op.remote_key, &im);
    if (s != Status::kOk) {
      r.status = s;
      break;
    }
    /* The same range check a NIC makes against an rkey. Without it this
     * process would read or write whatever the peer allocated next and
     * report success. */
    if (op.remote_addr < im->peer_addr) {
      r.status = Status::kInvalidArgument;
      break;
    }
    uint64_t const offset = op.remote_addr - im->peer_addr;
    if (offset > im->length || op.length > im->length - offset) {
      r.status = Status::kInvalidArgument;
      break;
    }

    void* remote = static_cast<char*>(im->mapped) + offset;
    void* dst = op.kind == SubOp::Kind::kRead ? op.local_addr : remote;
    void const* src = op.kind == SubOp::Kind::kRead ? remote : op.local_addr;
    /* A mapping that resolved to this process's own memory would make the
     * copy a no-op, and a transfer that moved nothing is indistinguishable
     * from one that never ran. Caught here rather than left to look like
     * data loss. */
    if (dst == src) {
      r.status = Status::kInternal;
      break;
    }
    Status cs = dev_->copy_nowait(dst, src, op.length);

    CompletionEvent ev;
    ev.request = op.request;
    ev.sub_id = op.sub_id;
    ev.bytes = cs == Status::kOk ? op.length : 0;
    ev.status = cs;
    /* A failed copy into the peer may have written part of the span before
     * failing, which the target owner has to be told. */
    ev.may_have_modified_target =
        cs != Status::kOk && op.kind == SubOp::Kind::kWrite;
    landed.push_back(ev);
    ++stats_.subops_posted;
    stats_.payload_bytes += op.length;
    /* Counted, because the mapping removes the network, not the copy. */
    if (cs == Status::kOk) stats_.payload_bytes_copied += op.length;
    ++r.accepted;
  }

  if (!landed.empty()) {
    Status ss = dev_->settle();
    if (ss != Status::kOk) {
      /* Nothing in this batch can be claimed to have landed. A write may
       * have partly reached the peer, which the target owner has to know. */
      for (auto& ev : landed) {
        if (ev.status != Status::kOk) continue;
        ev.status = ss;
        ev.bytes = 0;
        ev.may_have_modified_target = true;
      }
      r.status = ss;
    }
    for (auto const& ev : landed) completions_.push_back(ev);
  }
  return r;
}

Status IpcProvider::poll(uint32_t max_events,
                         std::vector<CompletionEvent>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  std::lock_guard<std::mutex> g(mu_);
  while (!completions_.empty() && out->size() < max_events) {
    auto const& ev = completions_.front();
    if (ev.status == Status::kOk)
      ++stats_.subops_completed;
    else
      ++stats_.subops_failed;
    out->push_back(ev);
    completions_.pop_front();
  }
  return Status::kOk;
}

Status IpcProvider::send_control(ProviderConnection* conn, uint16_t type,
                                 std::vector<uint8_t> const& payload) {
  if (conn == nullptr) return Status::kInvalidArgument;
  std::vector<uint8_t> body;
  put_u16(&body, type);
  body.insert(body.end(), payload.begin(), payload.end());
  std::lock_guard<std::mutex> g(mu_);
  if (peer_gone_) return Status::kPeerDisconnected;
  /* A send that fails means the same thing a closed read does. */
  Status s = send_frame(sock_, kFrameControl, body);
  if (s == Status::kPeerDisconnected) peer_is_gone_locked();
  return s;
}

Status IpcProvider::poll_control(uint32_t max_items,
                                 std::vector<ControlMessage>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  int fd = -1;
  {
    std::lock_guard<std::mutex> g(mu_);
    fd = sock_;
  }
  pump(fd);
  std::lock_guard<std::mutex> g(mu_);
  while (!control_.empty() && out->size() < max_items) {
    out->push_back(std::move(control_.front()));
    control_.pop_front();
  }
  return Status::kOk;
}

Status IpcProvider::poll_peer_arrivals(uint32_t,
                                       std::vector<PeerArrival>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  return Status::kOk;
}

Status IpcProvider::flush(ProviderConnection*) { return Status::kOk; }

bool IpcProvider::peer_alive() const {
  std::lock_guard<std::mutex> g(mu_);
  return !peer_gone_ && sock_ >= 0;
}

Status IpcProvider::drain(ProviderConnection*, int64_t) {
  /* The copy has finished by the time submit returns, so there is nothing in
   * flight to wait for. */
  return Status::kOk;
}

}  // namespace hux
