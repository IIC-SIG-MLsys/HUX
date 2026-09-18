/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "core/engine_impl.h"

#include <algorithm>
#include <chrono>

#include "control/control_message.h"
#include "core/ready_event_impl.h"
#include "hux/device.h"

namespace hux {

// ---------------- PeerImpl ----------------

PeerImpl::PeerImpl(PeerId id, ProviderConnectionPtr conn, PeerCaps caps,
                   EngineImpl* engine)
    : id_(id), conn_(std::move(conn)), caps_(caps), engine_(engine) {}

Status PeerImpl::import_region(std::vector<uint8_t> const& descriptor,
                               RemoteRegionPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  RegionDescriptor d;
  Status s = decode_descriptor(descriptor, &d);
  if (s != Status::kOk) return s;

  auto r = std::make_shared<RemoteRegionImpl>(d);
  {
    std::lock_guard<std::mutex> g(engine_->mu_);
    engine_->remotes_[d.region] = r;
  }
  {
    std::lock_guard<std::mutex> g(mu_);
    imported_.push_back(r);
  }
  *out = r;
  return Status::kOk;
}

Status PeerImpl::import_region_batch(
    std::vector<std::vector<uint8_t>> const& descs,
    std::vector<RemoteRegionPtr>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  out->reserve(descs.size());
  /* Per item: a malformed entry does not affect the rest; the caller spots
   * failures by the null pointer. */
  for (auto const& d : descs) {
    RemoteRegionPtr r;
    if (import_region(d, &r) != Status::kOk) r = nullptr;
    out->push_back(std::move(r));
  }
  return Status::kOk;
}

// ---------------- EngineImpl ----------------

EngineImpl::EngineImpl(EngineConfig cfg, std::shared_ptr<DeviceBackend> device,
                       TransportProviderPtr provider)
    : cfg_(std::move(cfg)),
      device_(std::move(device)),
      provider_(std::move(provider)) {
  if (cfg_.progress == ProgressMode::kThread) {
    progress_thread_ = std::thread([this] { progress_loop(); });
  }
}

EngineImpl::~EngineImpl() {
  stopping_.store(true, std::memory_order_release);
  if (progress_thread_.joinable()) progress_thread_.join();
}

Status Engine::create(EngineConfig const& cfg,
                      std::shared_ptr<DeviceBackend> /*device*/,
                      std::unique_ptr<Engine>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  std::string reason;
  Status s = cfg.validate(&reason);
  if (s != Status::kOk) return s;
  return Status::kInvalidArgument; /* Provider is injected; see make_engine. */
}

/* Construction entry used by tests and the upper factory: the provider is
 * injected so a mock can take its place. */
Status make_engine(EngineConfig const& cfg,
                   std::shared_ptr<DeviceBackend> device,
                   TransportProviderPtr provider,
                   std::unique_ptr<Engine>* out) {
  if (out == nullptr || provider == nullptr) return Status::kInvalidArgument;
  std::string reason;
  Status s = cfg.validate(&reason);
  if (s != Status::kOk) return s;
  *out = std::unique_ptr<Engine>(
      new EngineImpl(cfg, std::move(device), std::move(provider)));
  return Status::kOk;
}

Status EngineImpl::register_memory(void* addr, uint64_t length,
                                   AccessFlags access, MemoryRegionPtr* out) {
  if (out == nullptr || addr == nullptr || length == 0)
    return Status::kInvalidArgument;

  DeviceId dev = cfg_.device;
  MemoryKind mem = MemoryKind::kHostPageable;
  if (device_ != nullptr) {
    /* The backend decides which device a pointer belongs to. */
    DeviceId probed;
    MemoryKind probed_kind;
    if (device_->probe_pointer(addr, &probed, &probed_kind) == Status::kOk) {
      dev = probed;
      mem = probed_kind;
    }
    /* Where a device caps registration (Cambricon MLU sits near 32 MiB),
     * exceeding it is refused rather than silently rerouted through staging
     * while the caller believes it transfers in place. */
    uint64_t const cap = device_->caps().max_registration_bytes;
    if (cap != 0 && length > cap) return Status::kResourceExhausted;
  }

  uint64_t lkey = 0, rkey = 0;
  Status s =
      provider_->register_region(addr, length, dev, access, &lkey, &rkey);
  if (s != Status::kOk) return s;

  RegionId id = next_region_.fetch_add(1, std::memory_order_relaxed);
  auto r = std::make_shared<MemoryRegionImpl>(
      id, generation_.load(std::memory_order_acquire), addr, length, dev, mem,
      access, lkey, rkey);
  {
    std::lock_guard<std::mutex> g(mu_);
    regions_[id] = r;
  }
  *out = r;
  return Status::kOk;
}

Status EngineImpl::register_memory_batch(std::vector<void*> const& addrs,
                                         std::vector<uint64_t> const& lengths,
                                         AccessFlags access,
                                         std::vector<RegistrationResult>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  if (addrs.size() != lengths.size()) return Status::kInvalidArgument;
  out->clear();
  out->reserve(addrs.size());
  /* Per-item results; a partial failure keeps the successful ones. */
  for (size_t i = 0; i < addrs.size(); ++i) {
    RegistrationResult r;
    r.status = register_memory(addrs[i], lengths[i], access, &r.region);
    out->push_back(std::move(r));
  }
  return Status::kOk;
}

Status EngineImpl::deregister_memory(MemoryRegionPtr region) {
  if (region == nullptr) return Status::kInvalidArgument;
  auto impl = std::static_pointer_cast<MemoryRegionImpl>(region);
  /* Step one: block new submissions. */
  impl->retire();
  /* Step two: refuse while in-flight requests may still reference it. */
  {
    std::lock_guard<std::mutex> g(mu_);
    if (!inflight_.empty()) return Status::kWouldBlock;
    regions_.erase(impl->id());
  }
  return provider_->deregister_region(impl->local_key());
}

Status EngineImpl::local_metadata(std::vector<uint8_t>* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  return provider_->local_metadata(out);
}

Status EngineImpl::add_peer(std::vector<uint8_t> const& metadata,
                            PeerPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  ProviderConnectionPtr conn;
  Status s = provider_->connect(metadata, &conn);
  if (s != Status::kOk) return s;

  PeerCaps caps;
  caps.provider = provider_->caps().name;
  caps.path = PathKind::kRdma;
  caps.qp_count = conn->qp_count();
  if (device_ != nullptr) {
    caps.remote_max_registration_bytes = device_->caps().max_registration_bytes;
  }

  PeerId id = next_peer_.fetch_add(1, std::memory_order_relaxed);
  auto p = std::make_shared<PeerImpl>(id, std::move(conn), caps, this);
  {
    std::lock_guard<std::mutex> g(mu_);
    peers_[id] = p;
  }
  *out = p;
  return Status::kOk;
}

Status EngineImpl::remove_peer(PeerPtr peer) {
  if (peer == nullptr) return Status::kInvalidArgument;
  auto p = std::static_pointer_cast<PeerImpl>(peer);
  p->bump_epoch();
  {
    std::lock_guard<std::mutex> g(mu_);
    peers_.erase(p->id());
  }
  return provider_->disconnect(p->conn_ptr());
}

std::shared_ptr<MemoryRegionImpl> EngineImpl::find_region(RegionId id) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = regions_.find(id);
  return it == regions_.end() ? nullptr : it->second;
}

std::shared_ptr<RemoteRegionImpl> EngineImpl::find_remote(RegionId id) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = remotes_.find(id);
  return it == remotes_.end() ? nullptr : it->second;
}

Status EngineImpl::build_subops(std::vector<RegionView> const& local,
                                std::vector<RegionView> const& remote,
                                SubOp::Kind kind, RequestId req,
                                std::vector<SubOp>* out,
                                std::vector<MemoryRegionPtr>* held,
                                void** target_addr, uint64_t* target_bytes) {
  uint64_t sub_id = 0;
  uint64_t total = 0;
  for (size_t i = 0; i < local.size(); ++i) {
    auto lr = find_region(local[i].region);
    if (lr == nullptr) return Status::kNotFound;
    if (lr->retired()) return Status::kStaleGeneration;
    auto rr = find_remote(remote[i].region);
    if (rr == nullptr) return Status::kNotFound;
    if (!rr->valid()) return Status::kStaleGeneration;

    /* Segments pair up by index and must have equal lengths. */
    if (local[i].span.length != remote[i].span.length)
      return Status::kInvalidArgument;
    if (!local[i].span.within(lr->length())) return Status::kOutOfRange;
    if (!remote[i].span.within(rr->length())) return Status::kOutOfRange;

    held->push_back(lr);
    total += local[i].span.length;

    /* Split by chunk_bytes. A chunk is the scheduling and pacing unit, which
     * is neither the application segment nor the backend's wr_batch. */
    uint64_t off = 0;
    while (off < local[i].span.length) {
      uint64_t n = local[i].span.length - off;
      if (n > cfg_.chunk_bytes) n = cfg_.chunk_bytes;
      SubOp op;
      op.kind = kind;
      op.request = req;
      op.sub_id = sub_id++;
      op.local_addr =
          static_cast<char*>(lr->base()) + local[i].span.offset + off;
      op.local_key = lr->local_key();
      op.remote_addr = rr->base() + remote[i].span.offset + off;
      op.remote_key = rr->remote_key();
      op.length = n;
      out->push_back(op);
      off += n;
    }
  }
  if (!local.empty()) {
    auto lr = find_region(local[0].region);
    if (lr != nullptr) {
      *target_addr = static_cast<char*>(lr->base()) + local[0].span.offset;
    }
  }
  *target_bytes = total;
  return Status::kOk;
}

bool EngineImpl::dependencies_met(
    std::vector<DeviceEventPtr> const& after) const {
  for (auto const& e : after) {
    if (e == nullptr) continue;
    /* Never recorded means it captures no work at all; waiting on it would
     * order nothing, so it can never count as satisfied. */
    if (!e->recorded()) return false;
    bool complete = false;
    if (e->query(&complete) != Status::kOk) return false;
    if (!complete) return false;
  }
  return true;
}

/* Hands sub-operations to the provider and records how many it took.
 *
 * At most max_bytes is offered in one turn. A provider that accepts only part
 * of what it is given is saying one of two things. Out of budget or queue
 * space means the rest should be offered again later, and dropping it would
 * lose data the caller believes is on its way. A real error means no more
 * will be accepted, and the request has to stop waiting for completions that
 * will never arrive.
 *
 * Returns true when nothing is left to submit for this request. */
bool EngineImpl::post_ops(PendingSubmit* p, uint64_t max_bytes) {
  if (p->ops.empty()) return true;

  /* Take whole sub-operations up to the quantum, and always at least one:
   * a quantum smaller than a chunk must still make progress rather than
   * stall. */
  size_t take = 0;
  uint64_t bytes = 0;
  while (take < p->ops.size()) {
    if (take > 0 && bytes + p->ops[take].length > max_bytes) break;
    bytes += p->ops[take].length;
    ++take;
  }

  std::vector<SubOp> slice(p->ops.begin(), p->ops.begin() + take);
  p->req->set_state(RequestState::kInflight);
  SubmitResult sr = provider_->submit(p->conn.get(), slice);
  p->req->add_accepted_subops(sr.accepted);

  p->ops.erase(p->ops.begin(), p->ops.begin() + sr.accepted);

  if (sr.accepted < slice.size() && sr.status == Status::kWouldBlock) {
    /* Deferred, not failed. */
    std::lock_guard<std::mutex> sg(stats_mu_);
    ++stats_.submit_deferred;
    return false;
  }

  if (sr.accepted == 0 && sr.status != Status::kOk) {
    ErrorInfo e;
    e.status = sr.status;
    e.provider = provider_->caps().name;
    e.peer_id = p->peer;
    e.provider_errno = sr.provider_errno;
    e.detail = "submit rejected";
    p->req->seal_accepted();
    {
      std::lock_guard<std::mutex> g(mu_);
      inflight_.erase(p->req->id());
      completed_.push_back(p->req);
    }
    p->req->fail(e);
    return true;
  }
  if (sr.accepted < slice.size()) {
    ErrorInfo e;
    e.status = sr.status == Status::kOk ? Status::kTransportError : sr.status;
    e.provider = provider_->caps().name;
    e.peer_id = p->peer;
    e.provider_errno = sr.provider_errno;
    e.may_have_modified_target = true;
    e.detail = "partial submit";
    p->req->seal_accepted();
    p->req->fail(e);
    return true;
  }
  return p->ops.empty();
}

/* One scheduling pass: every waiting request gets a turn of at most a
 * quantum, starting from a rotating position so none of them is permanently
 * first. */
void EngineImpl::drain_pending() {
  std::vector<PendingSubmit> turn;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (pending_.empty()) return;
    size_t const n = pending_.size();
    if (rr_cursor_ >= n) rr_cursor_ = 0;
    /* Rotate so the pass starts at a different request each time. */
    std::rotate(pending_.begin(), pending_.begin() + rr_cursor_,
                pending_.end());
    turn.assign(std::make_move_iterator(pending_.begin()),
                std::make_move_iterator(pending_.end()));
    pending_.clear();
    rr_cursor_ = n > 1 ? 1 : 0;
  }

  std::vector<PendingSubmit> still_waiting;
  for (auto& p : turn) {
    if (!dependencies_met(p.after)) {
      still_waiting.push_back(std::move(p));
      continue;
    }
    if (!post_ops(&p, cfg_.scheduler_quantum_bytes))
      still_waiting.push_back(std::move(p));
  }

  if (!still_waiting.empty()) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& p : still_waiting) pending_.push_back(std::move(p));
  }
}

Status EngineImpl::submit_vector(Peer* peer,
                                 std::vector<RegionView> const& local,
                                 std::vector<RegionView> const& remote,
                                 TransferOptions const& opts, SubOp::Kind kind,
                                 RequestPtr* out) {
  if (out == nullptr || peer == nullptr) return Status::kInvalidArgument;
  if (closed_.load(std::memory_order_acquire)) return Status::kInvalidArgument;
  if (local.empty() || local.size() != remote.size())
    return Status::kInvalidArgument;

  auto* p = static_cast<PeerImpl*>(peer);
  if (!p->connected()) return Status::kPeerDisconnected;

  /* Bounded submission queue. kWouldBlock means the request was not accepted
   * and had no network side effect, so it can be retried as is. */
  {
    std::lock_guard<std::mutex> g(mu_);
    if (inflight_.size() >= cfg_.max_inflight_requests) {
      std::lock_guard<std::mutex> sg(stats_mu_);
      ++stats_.requests_would_block;
      return Status::kWouldBlock;
    }
  }

  RequestId req_id = next_request_.fetch_add(1, std::memory_order_relaxed);
  std::vector<SubOp> ops;
  std::vector<MemoryRegionPtr> held;
  void* target_addr = nullptr;
  uint64_t target_bytes = 0;
  Status s = build_subops(local, remote, kind, req_id, &ops, &held,
                          &target_addr, &target_bytes);
  if (s != Status::kOk) return s;

  auto req = std::make_shared<RequestImpl>(
      req_id, kind, static_cast<uint32_t>(ops.size()), opts.context);
  for (auto& r : held) req->hold_region(std::move(r));
  req->hold_connection(p->conn_ptr());
  req->set_device_backend(device_.get());
  req->set_target(target_addr, target_bytes);
  if (kind == SubOp::Kind::kWrite && !remote.empty()) {
    auto rr = find_remote(remote[0].region);
    if (rr != nullptr) {
      req->set_remote_target(remote[0].region, rr->generation(),
                             Span{remote[0].span.offset, target_bytes});
    }
  }

  {
    std::lock_guard<std::mutex> g(mu_);
    inflight_[req_id] = req;
  }
  {
    std::lock_guard<std::mutex> g(stats_mu_);
    ++stats_.requests_accepted;
  }

  PendingSubmit ps;
  ps.req = req;
  ps.ops = std::move(ops);
  ps.conn = p->conn_ptr();
  ps.peer = p->id();
  ps.after = opts.after;

  if (!ps.after.empty() && !dependencies_met(ps.after)) {
    /* The request is accepted and handed back now; only its submission waits,
     * so the calling thread never blocks on the device. */
    req->set_state(RequestState::kWaitDependency);
    {
      std::lock_guard<std::mutex> g(mu_);
      pending_.push_back(std::move(ps));
    }
    *out = req;
    return Status::kOk;
  }

  if (!post_ops(&ps, cfg_.scheduler_quantum_bytes)) {
    /* Not all of it fit in one turn; the rest waits for a later pass. */
    std::lock_guard<std::mutex> g(mu_);
    pending_.push_back(std::move(ps));
  }
  *out = req;
  if (req->accepted_subops() > 0 || req->error().ok()) return Status::kOk;
  return req->error().status;
}

Status EngineImpl::read(Peer* peer, RegionView const& local,
                        RegionView const& remote, TransferOptions const& opts,
                        RequestPtr* out) {
  /* Scalar is a single-segment shortcut over the same path. */
  return submit_vector(peer, {local}, {remote}, opts, SubOp::Kind::kRead, out);
}

Status EngineImpl::write(Peer* peer, RegionView const& local,
                         RegionView const& remote, TransferOptions const& opts,
                         RequestPtr* out) {
  return submit_vector(peer, {local}, {remote}, opts, SubOp::Kind::kWrite, out);
}

Status EngineImpl::readv(Peer* peer, std::vector<RegionView> const& local,
                         std::vector<RegionView> const& remote,
                         TransferOptions const& opts, RequestPtr* out) {
  return submit_vector(peer, local, remote, opts, SubOp::Kind::kRead, out);
}

Status EngineImpl::writev(Peer* peer, std::vector<RegionView> const& local,
                          std::vector<RegionView> const& remote,
                          TransferOptions const& opts, RequestPtr* out) {
  return submit_vector(peer, local, remote, opts, SubOp::Kind::kWrite, out);
}

Status EngineImpl::notify(Peer* peer, std::vector<uint8_t> const& payload,
                          RequestPtr* out) {
  if (peer == nullptr || out == nullptr) return Status::kInvalidArgument;
  if (payload.size() > cfg_.notify_max_payload) return Status::kInvalidArgument;
  if (closed_.load(std::memory_order_acquire)) return Status::kInvalidArgument;

  auto* p = static_cast<PeerImpl*>(peer);
  if (!p->connected()) return Status::kPeerDisconnected;

  uint64_t id = next_notify_.fetch_add(1, std::memory_order_relaxed);
  RequestId req_id = next_request_.fetch_add(1, std::memory_order_relaxed);
  /* A notification is its own kind of request: no sub-operations, and it
   * completes on the peer's acknowledgement rather than on any CQE. */
  auto req =
      std::make_shared<RequestImpl>(req_id, SubOp::Kind::kWrite, 0, nullptr);
  req->set_state(RequestState::kWaitNotifyAck);

  std::vector<uint8_t> body(kNotificationHeaderBytes + payload.size());
  std::vector<uint8_t> idbuf;
  encode_u64(id, &idbuf);
  std::copy(idbuf.begin(), idbuf.end(), body.begin());
  std::copy(payload.begin(), payload.end(),
            body.begin() + kNotificationHeaderBytes);

  {
    std::lock_guard<std::mutex> g(mu_);
    notify_pending_[id] = req;
  }

  Status s = provider_->send_control(
      p->conn(), static_cast<uint16_t>(ControlType::kNotification), body);
  if (s != Status::kOk) {
    {
      std::lock_guard<std::mutex> g(mu_);
      notify_pending_.erase(id);
    }
    ErrorInfo e;
    e.status = s;
    e.provider = provider_->caps().name;
    e.peer_id = p->id();
    e.detail = "control send failed";
    req->fail(e);
  }
  *out = req;
  return s;
}

Status EngineImpl::poll_notifications(uint32_t max_items,
                                      std::vector<Notification>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  /* Drives progress in explicit mode for the same reason poll_completions
   * does: a caller polling only for notifications would otherwise never
   * receive one. */
  if (cfg_.progress == ProgressMode::kExplicit) progress();
  std::lock_guard<std::mutex> g(mu_);
  while (!notifications_.empty() && out->size() < max_items) {
    out->push_back(std::move(notifications_.front()));
    notifications_.pop_front();
  }
  return Status::kOk;
}

Status EngineImpl::poll_completions(uint32_t max_items,
                                    std::vector<RequestPtr>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  if (cfg_.progress == ProgressMode::kExplicit) {
    Status s = progress();
    if (s != Status::kOk) return s;
  }
  std::lock_guard<std::mutex> g(mu_);
  while (!completed_.empty() && out->size() < max_items) {
    out->push_back(std::move(completed_.front()));
    completed_.pop_front();
  }
  return Status::kOk;
}

Status EngineImpl::poll_ready_events(uint32_t max_items,
                                     std::vector<ReadyEventPtr>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  if (cfg_.progress == ProgressMode::kExplicit) progress();
  std::lock_guard<std::mutex> g(mu_);
  while (!ready_events_.empty() && out->size() < max_items) {
    out->push_back(std::move(ready_events_.front()));
    ready_events_.pop_front();
  }
  return Status::kOk;
}

Status EngineImpl::progress() {
  /* One scheduling pass first, so a request that has become ready is offered
   * in this same call rather than a later one. */
  drain_pending();

  {
    std::vector<ControlMessage> msgs;
    if (provider_->poll_control(cfg_.cq_batch, &msgs) == Status::kOk) {
      for (auto const& m : msgs) {
        if (static_cast<ControlType>(m.type) == ControlType::kReadyHandoff) {
          ReadyHandoffBody b;
          if (decode_ready_handoff(m.payload, &b) != Status::kOk) continue;
          void* addr = nullptr;
          auto lr = find_region(b.region);
          /* The peer names a region by the id it imported; only a region this
           * engine actually holds can be handed to a consumer. */
          if (lr != nullptr && Span{b.offset, b.length}.within(lr->length())) {
            addr = static_cast<char*>(lr->base()) + b.offset;
          }
          std::lock_guard<std::mutex> g(mu_);
          ready_events_.push_back(std::make_shared<ReadyEventImpl>(
              b.request, m.peer, device_.get(), addr, b.length, b.region,
              b.generation, Span{b.offset, b.length}));
        } else if (static_cast<ControlType>(m.type) ==
                   ControlType::kNotification) {
          if (m.payload.size() < kNotificationHeaderBytes) continue;
          std::vector<uint8_t> idbuf(
              m.payload.begin(), m.payload.begin() + kNotificationHeaderBytes);
          uint64_t id = 0;
          if (decode_u64(idbuf, &id) != Status::kOk) continue;

          bool queued = false;
          {
            std::lock_guard<std::mutex> g(mu_);
            if (notifications_.size() < cfg_.notify_queue_depth) {
              Notification n;
              n.peer = m.peer;
              n.id = id;
              n.payload.assign(m.payload.begin() + kNotificationHeaderBytes,
                               m.payload.end());
              notifications_.push_back(std::move(n));
              queued = true;
            }
          }
          /* Acknowledged only once it is actually in the queue. Confirming a
           * message that was dropped would tell the sender something false,
           * and back-pressure depends on the sender learning the truth. */
          if (queued && m.conn != nullptr) {
            std::vector<uint8_t> ack;
            encode_u64(id, &ack);
            provider_->send_control(
                m.conn, static_cast<uint16_t>(ControlType::kNotificationAck),
                ack);
          }
        } else if (static_cast<ControlType>(m.type) ==
                   ControlType::kNotificationAck) {
          uint64_t id = 0;
          if (decode_u64(m.payload, &id) != Status::kOk) continue;
          RequestImplPtr req;
          {
            std::lock_guard<std::mutex> g(mu_);
            auto it = notify_pending_.find(id);
            if (it != notify_pending_.end()) {
              req = it->second;
              notify_pending_.erase(it);
            }
          }
          if (req != nullptr) {
            req->mark_stage(Stage::kTransferComplete);
            req->finish_success();
            std::lock_guard<std::mutex> g(mu_);
            completed_.push_back(req);
          }
        }
      }
    }
  }

  {
    std::vector<PeerArrival> arrivals;
    if (provider_->poll_peer_arrivals(cfg_.cq_batch, &arrivals) ==
            Status::kOk &&
        !arrivals.empty()) {
      std::lock_guard<std::mutex> g(mu_);
      for (auto const& a : arrivals) {
        ready_events_.push_back(std::make_shared<ReadyEventImpl>(
            a.token, a.from, device_.get(), nullptr, 0));
      }
    }
  }

  std::vector<CompletionEvent> events;
  /* Take and handle the whole batch. The provider contract requires every
   * event it collected; returning early drops other requests' completions. */
  Status s = provider_->poll(cfg_.cq_batch, &events);
  if (s != Status::kOk) return s;

  for (auto const& ev : events) {
    RequestImplPtr req;
    {
      std::lock_guard<std::mutex> g(mu_);
      auto it = inflight_.find(ev.request);
      if (it == inflight_.end()) continue; /* Terminal or already taken. */
      req = it->second;
    }
    bool last = req->on_subop_complete(ev);
    if (!last) continue;

    if (req->cancel_requested()) {
      req->finish_cancelled();
      std::lock_guard<std::mutex> g(stats_mu_);
      ++stats_.requests_cancelled;
    } else if (!req->error().ok()) {
      req->fail(req->error());
      std::lock_guard<std::mutex> g(stats_mu_);
      ++stats_.requests_failed;
    } else {
      /* Every accepted sub-operation is done, so transfer_complete holds.
       * Device visibility comes next, and only then target_ready. */
      req->mark_stage(Stage::kTransferComplete);
      if (device_ != nullptr && req->kind() == SubOp::Kind::kRead) {
        /* A direct write to device memory orders nothing against a consuming
         * kernel on its own. */
        req->mark_stage(Stage::kTargetReady);
      }
      if (req->kind() == SubOp::Kind::kWrite && req->remote_region() != 0) {
        /* The peer knows bytes arrived from the immediate value, but not
         * which ones. This says so, and travels on the control channel so a
         * congested data path cannot delay it. */
        ReadyHandoffBody b;
        b.request = req->id();
        b.region = req->remote_region();
        b.generation = req->remote_generation();
        b.offset = req->remote_span().offset;
        b.length = req->remote_span().length;
        std::vector<uint8_t> payload;
        encode_ready_handoff(b, &payload);
        ProviderConnectionPtr conn = req->connection();
        if (conn != nullptr) {
          provider_->send_control(
              conn.get(), static_cast<uint16_t>(ControlType::kReadyHandoff),
              payload);
        }
      }
      req->finish_success();
      std::lock_guard<std::mutex> g(stats_mu_);
      ++stats_.requests_succeeded;
    }
    {
      std::lock_guard<std::mutex> g(mu_);
      inflight_.erase(ev.request);
      completed_.push_back(req);
    }
  }
  return Status::kOk;
}

void EngineImpl::progress_loop() {
  while (!stopping_.load(std::memory_order_acquire)) {
    progress();
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

EngineStats EngineImpl::stats() const {
  EngineStats s;
  {
    std::lock_guard<std::mutex> g(stats_mu_);
    s = stats_;
  }
  {
    std::lock_guard<std::mutex> g(mu_);
    s.requests_waiting_on_dependency = pending_.size();
  }
  /* Sub-operation and byte counts come from the provider, which is the only
   * layer that knows whether a copy happened. */
  ProviderStats ps = provider_->stats();
  s.subops_posted = ps.subops_posted;
  s.subops_completed = ps.subops_completed;
  s.subops_failed = ps.subops_failed;
  s.payload_bytes = ps.payload_bytes;
  s.payload_bytes_copied = ps.payload_bytes_copied;
  return s;
}

Status EngineImpl::record_event(DeviceStream* stream, DeviceEventPtr* out) {
  if (device_ == nullptr) return Status::kUnsupported;
  return device_->record_event(stream, out);
}

Status EngineImpl::close(int64_t timeout_ms) {
  closed_.store(true, std::memory_order_release);
  auto const deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
  while (true) {
    {
      std::lock_guard<std::mutex> g(mu_);
      if (inflight_.empty()) break;
    }
    if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) {
      /* On timeout keep the resources and report the incomplete state; never
       * destroy objects that DMA may still touch. */
      return Status::kTimeout;
    }
    progress();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  stopping_.store(true, std::memory_order_release);
  if (progress_thread_.joinable()) progress_thread_.join();
  return Status::kOk;
}

}  // namespace hux
