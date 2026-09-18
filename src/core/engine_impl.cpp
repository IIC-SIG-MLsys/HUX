/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "core/engine_impl.h"

#include <chrono>

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
    : cfg_(std::move(cfg)), device_(std::move(device)),
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
  return Status::kInvalidArgument;  /* Provider is injected; see make_engine. */
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
  Status s = provider_->register_region(addr, length, dev, access, &lkey, &rkey);
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

Status EngineImpl::register_memory_batch(
    std::vector<void*> const& addrs, std::vector<uint64_t> const& lengths,
    AccessFlags access, std::vector<RegistrationResult>* out) {
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

Status EngineImpl::add_peer(std::vector<uint8_t> const& metadata, PeerPtr* out) {
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

/* Hands the sub-operations to the provider and records how many it took. */
void EngineImpl::post_ops(PendingSubmit* p) {
  p->req->set_state(RequestState::kInflight);
  SubmitResult sr = provider_->submit(p->conn.get(), p->ops);
  p->req->set_accepted_subops(sr.accepted);

  if (sr.accepted == 0 && sr.status != Status::kOk) {
    ErrorInfo e;
    e.status = sr.status;
    e.provider = provider_->caps().name;
    e.peer_id = p->peer;
    e.provider_errno = sr.provider_errno;
    e.detail = "submit rejected";
    {
      std::lock_guard<std::mutex> g(mu_);
      inflight_.erase(p->req->id());
      completed_.push_back(p->req);
    }
    p->req->fail(e);
    return;
  }
  if (sr.accepted < p->ops.size()) {
    ErrorInfo e;
    e.status = sr.status == Status::kOk ? Status::kTransportError : sr.status;
    e.provider = provider_->caps().name;
    e.peer_id = p->peer;
    e.provider_errno = sr.provider_errno;
    e.may_have_modified_target = true;
    e.detail = "partial submit";
    p->req->fail(e);
  }
}

Status EngineImpl::submit_vector(Peer* peer, std::vector<RegionView> const& local,
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

  if (kind == SubOp::Kind::kWrite && !ops.empty() &&
      provider_->caps().supports_peer_signal) {
    /* One signal per logical request, on its final sub-operation: a write is
     * invisible to the receiving CPU otherwise, and signalling every chunk
     * would cost a receive each time. */
    ops.back().signal_peer = true;
    ops.back().peer_token = static_cast<uint32_t>(req_id);
  }

  auto req = std::make_shared<RequestImpl>(
      req_id, kind, static_cast<uint32_t>(ops.size()), opts.context);
  for (auto& r : held) req->hold_region(std::move(r));
  req->hold_connection(p->conn_ptr());
  req->set_device_backend(device_.get());
  req->set_target(target_addr, target_bytes);

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

  post_ops(&ps);
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
                          RequestPtr* out) {  // NOLINT
  if (peer == nullptr || out == nullptr) return Status::kInvalidArgument;
  if (payload.size() > cfg_.notify_max_payload) return Status::kInvalidArgument;
  return Status::kUnsupported;  /* NTF-01, milestone M4. */
}

Status EngineImpl::poll_notifications(uint32_t max_items,
                                      std::vector<Notification>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
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
  /* Release anything whose device dependencies have since been met. Done
   * before polling so a request that becomes ready is posted in this same
   * pass rather than a later one. */
  {
    std::vector<PendingSubmit> ready;
    {
      std::lock_guard<std::mutex> g(mu_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (dependencies_met(it->after)) {
          ready.push_back(std::move(*it));
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& p : ready) post_ops(&p);
  }

  {
    std::vector<PeerArrival> arrivals;
    if (provider_->poll_peer_arrivals(cfg_.cq_batch, &arrivals) == Status::kOk &&
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
      if (it == inflight_.end()) continue;  /* Terminal or already taken. */
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
      std::chrono::steady_clock::now() + std::chrono::milliseconds(
          timeout_ms < 0 ? 0 : timeout_ms);
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
