/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "transport/local/local_provider.h"

#include <cstring>

namespace hux {
namespace {

/* Enough of a connection object to carry the contract. There is no queue pair
 * to account for, but capacity is still bounded: a local path that accepts
 * without limit would let a caller build a backlog it cannot see. */
class LocalConnection : public ProviderConnection {
 public:
  explicit LocalConnection(LocalProvider* owner) : owner_(owner) {}
  uint32_t qp_count() const override { return 1; }
  uint32_t submit_capacity() const override { return 4096; }
  LocalProvider* owner() const { return owner_; }

 private:
  LocalProvider* owner_;
};

}  // namespace

LocalRegistry& LocalRegistry::instance() {
  static LocalRegistry r;
  return r;
}

uint64_t LocalRegistry::publish(void* addr, uint64_t length) {
  std::unique_lock<std::shared_mutex> g(mu_);
  uint64_t key = next_++;
  entries_[key] = Entry{addr, length};
  return key;
}

void LocalRegistry::withdraw(uint64_t key) {
  std::unique_lock<std::shared_mutex> g(mu_);
  entries_.erase(key);
}

void* LocalRegistry::resolve(uint64_t key, uint64_t address,
                             uint64_t length) const {
  std::shared_lock<std::shared_mutex> g(mu_);
  return resolve_locked(key, address, length);
}

bool LocalRegistry::copy(uint64_t key, uint64_t address, uint64_t length,
                         void* local, bool into_local) const {
  std::shared_lock<std::shared_mutex> g(mu_);
  void* remote = resolve_locked(key, address, length);
  if (remote == nullptr) return false;
  if (into_local)
    std::memcpy(local, remote, static_cast<size_t>(length));
  else
    std::memcpy(remote, local, static_cast<size_t>(length));
  return true;
}

void* LocalRegistry::resolve_locked(uint64_t key, uint64_t address,
                                    uint64_t length) const {
  auto it = entries_.find(key);
  if (it == entries_.end()) return nullptr;

  auto const base = reinterpret_cast<uintptr_t>(it->second.addr);
  if (address < base) return nullptr;
  uint64_t const offset = address - base;
  /* Compared before adding, so a wrapping range cannot appear to fit. The
   * check exists for the same reason a NIC refuses an out-of-range access:
   * without it the process would read whatever follows in its own address
   * space and report success. */
  if (offset > it->second.length || length > it->second.length - offset)
    return nullptr;
  return static_cast<char*>(it->second.addr) + offset;
}

Status LocalProvider::create(std::shared_ptr<LocalProvider>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  *out = std::shared_ptr<LocalProvider>(new LocalProvider());
  return Status::kOk;
}

void LocalProvider::pair_with(std::shared_ptr<LocalProvider> peer) {
  std::lock_guard<std::mutex> g(mu_);
  peer_ = peer;
}

ProviderCaps LocalProvider::caps() const {
  ProviderCaps c;
  c.name = "local";
  c.supports_read = true;
  c.supports_write = true;
  c.supports_vector = true;
  c.supports_multi_qp = false;
  c.needs_explicit_flush = false;
  c.supports_peer_signal = false;
  c.max_segment_bytes = 0;
  c.max_sge = 1;
  return c;
}

ProviderStats LocalProvider::stats() const {
  std::lock_guard<std::mutex> g(mu_);
  return stats_;
}

std::string LocalProvider::describe() const {
  return "{\"provider\":\"local\",\"path\":\"same_process\"}";
}

Status LocalProvider::register_region(void* addr, uint64_t length, DeviceId,
                                      AccessFlags, uint64_t* local_key,
                                      uint64_t* remote_key) {
  if (addr == nullptr || length == 0 || local_key == nullptr ||
      remote_key == nullptr)
    return Status::kInvalidArgument;
  uint64_t const key = LocalRegistry::instance().publish(addr, length);
  {
    std::lock_guard<std::mutex> g(mu_);
    local_keys_[key] = addr;
  }
  /* Both keys are the same here. On a NIC they differ, and a test that
   * assumed otherwise would pass locally and fail on hardware -- which is why
   * the mock deliberately makes them differ. */
  *local_key = key;
  *remote_key = key;
  return Status::kOk;
}

Status LocalProvider::deregister_region(uint64_t local_key) {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (local_keys_.erase(local_key) == 0) return Status::kNotFound;
  }
  LocalRegistry::instance().withdraw(local_key);
  return Status::kOk;
}

Status LocalProvider::connect(std::vector<uint8_t> const&,
                              ProviderConnectionPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  *out = std::make_shared<LocalConnection>(this);
  return Status::kOk;
}

Status LocalProvider::disconnect(ProviderConnectionPtr) { return Status::kOk; }

Status LocalProvider::local_metadata(std::vector<uint8_t>* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  *out = {'l', 'o', 'c', 'a', 'l'};
  return Status::kOk;
}

SubmitResult LocalProvider::submit(ProviderConnection* conn,
                                   std::vector<SubOp> const& ops) {
  SubmitResult r;
  if (conn == nullptr) {
    r.status = Status::kInvalidArgument;
    return r;
  }

  for (auto const& op : ops) {
    /* The remote address is a key plus an offset, resolved through the
     * registry, not a pointer taken on trust. Dereferencing what a peer sent
     * would work here and be a serious bug the moment the peer is remote. */
    if (!LocalRegistry::instance().copy(op.remote_key, op.remote_addr,
                                        op.length, op.local_addr,
                                        op.kind == SubOp::Kind::kRead)) {
      /* The same refusal a NIC gives for an unknown or out-of-range key. */
      r.status = Status::kInvalidArgument;
      break;
    }

    CompletionEvent ev;
    ev.request = op.request;
    ev.sub_id = op.sub_id;
    ev.bytes = op.length;
    ev.status = Status::kOk;
    {
      std::lock_guard<std::mutex> g(mu_);
      completions_.push_back(ev);
      ++stats_.subops_posted;
      stats_.payload_bytes += op.length;
      /* Counted as a copy, because it is one. Claiming zero here would make
       * the local path look like the in-place one it is not. */
      stats_.payload_bytes_copied += op.length;
    }
    ++r.accepted;
  }
  return r;
}

Status LocalProvider::poll(uint32_t max_events,
                           std::vector<CompletionEvent>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  std::lock_guard<std::mutex> g(mu_);
  while (!completions_.empty() && out->size() < max_events) {
    out->push_back(completions_.front());
    ++stats_.subops_completed;
    completions_.pop_front();
  }
  return Status::kOk;
}

void LocalProvider::deliver_control(ControlMessage msg) {
  std::lock_guard<std::mutex> g(mu_);
  control_.push_back(std::move(msg));
}

Status LocalProvider::send_control(ProviderConnection* conn, uint16_t type,
                                   std::vector<uint8_t> const& payload) {
  if (conn == nullptr) return Status::kInvalidArgument;
  std::shared_ptr<LocalProvider> peer;
  {
    std::lock_guard<std::mutex> g(mu_);
    peer = peer_.lock();
  }
  ControlMessage m;
  m.type = type;
  m.payload = payload;
  m.conn = conn;
  /* Unpaired, the message loops back, which is what a single engine talking
   * to itself needs. Paired, it goes to the other side. */
  if (peer != nullptr)
    peer->deliver_control(std::move(m));
  else
    deliver_control(std::move(m));
  return Status::kOk;
}

Status LocalProvider::poll_control(uint32_t max_items,
                                   std::vector<ControlMessage>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  std::lock_guard<std::mutex> g(mu_);
  while (!control_.empty() && out->size() < max_items) {
    out->push_back(std::move(control_.front()));
    control_.pop_front();
  }
  return Status::kOk;
}

Status LocalProvider::poll_peer_arrivals(uint32_t,
                                         std::vector<PeerArrival>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  return Status::kOk;
}

Status LocalProvider::flush(ProviderConnection*) { return Status::kOk; }

Status LocalProvider::drain(ProviderConnection*, int64_t) {
  /* A memcpy has finished by the time submit returns, so there is nothing in
   * flight to wait for. */
  return Status::kOk;
}

}  // namespace hux
