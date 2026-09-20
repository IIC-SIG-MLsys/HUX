/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "core/engine_impl.h"

#include <algorithm>
#include <chrono>

#include "control/control_message.h"
#include "control/identity.h"
#include "core/ready_event_impl.h"
#include "hux/device.h"

namespace hux {

// ---------------- PeerImpl ----------------

PeerImpl::PeerImpl(PeerId id, ProviderConnectionPtr conn, PeerCaps caps,
                   EngineImpl* engine, TransportProviderPtr provider)
    : id_(id),
      conn_(std::move(conn)),
      provider_(std::move(provider)),
      caps_(caps),
      engine_(engine) {}

Status PeerImpl::import_region(std::vector<uint8_t> const& descriptor,
                               RemoteRegionPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  RegionDescriptor d;
  Status s = decode_descriptor(descriptor, &d);
  if (s != Status::kOk) return s;

  /* The peer exported a key per transport; take the one minted by the
   * transport this peer is reached over. A region exported only for another
   * path is refused here, where the reason is visible, rather than accepted
   * and rejected by hardware somewhere with no context. */
  if (!d.provider_keys.empty()) {
    std::string const want = provider_->caps().name;
    bool found = false;
    for (auto const& k : d.provider_keys) {
      if (k.provider == want) {
        d.remote_key = k.remote_key;
        found = true;
        break;
      }
    }
    if (!found) return Status::kUnsupported;
  }

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
                       std::vector<TransportProviderPtr> providers)
    : cfg_(std::move(cfg)),
      device_(std::move(device)),
      providers_(std::move(providers)),
      engine_id_(next_engine_id()) {
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
  std::vector<TransportProviderPtr> providers;
  providers.push_back(std::move(provider));
  *out = std::unique_ptr<Engine>(
      new EngineImpl(cfg, std::move(device), std::move(providers)));
  return Status::kOk;
}

Status make_engine(EngineConfig const& cfg,
                   std::shared_ptr<DeviceBackend> device,
                   std::vector<TransportProviderPtr> providers,
                   std::unique_ptr<Engine>* out) {
  if (out == nullptr || providers.empty()) return Status::kInvalidArgument;
  for (auto const& p : providers)
    if (p == nullptr) return Status::kInvalidArgument;
  std::string reason;
  Status s = cfg.validate(&reason);
  if (s != Status::kOk) return s;
  *out = std::unique_ptr<Engine>(
      new EngineImpl(cfg, std::move(device), std::move(providers)));
  return Status::kOk;
}

/* Where a peer is decides which transport can reach it. A peer in this
 * process or on this host is served by a provider that says it can do that;
 * anything else goes to the first provider, which is the network one in every
 * configuration that has one. */
TransportProviderPtr EngineImpl::provider_for(Locality locality) const {
  if (locality == Locality::kSameHost || locality == Locality::kSameProcess) {
    for (auto const& p : providers_) {
      std::string const name = p->caps().name;
      if (name == "ipc" ||
          (locality == Locality::kSameProcess && name == "local"))
        return p;
    }
  }
  return providers_.front();
}

RegistrationPtr EngineImpl::find_registration(void* addr, uint64_t length,
                                              DeviceId device,
                                              AccessFlags access) {
  std::lock_guard<std::mutex> g(mu_);
  for (auto const& r : reg_cache_) {
    if (registration_covers(*r, addr, length, device, access)) return r;
  }
  return nullptr;
}

void EngineImpl::cache_registration(RegistrationPtr reg) {
  std::lock_guard<std::mutex> g(mu_);
  reg_cache_.push_back(std::move(reg));
  /* Over the bound, drop entries nothing else is holding. One still in use is
   * skipped rather than evicted: releasing it would pull the registration out
   * from under a handle that may still be transferring. */
  while (reg_cache_.size() > cfg_.registration_cache_entries) {
    bool evicted = false;
    for (auto it = reg_cache_.begin(); it != reg_cache_.end(); ++it) {
      if (it->use_count() == 1) {
        reg_cache_.erase(it);
        evicted = true;
        break;
      }
    }
    if (!evicted) break; /* everything is in use; the bound gives way */
  }
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

  /* An existing registration that already covers this range is reused. The
   * handle stays its own: it describes what the caller asked for, holds an
   * independent reference, and is deregistered on its own. */
  RegistrationPtr reg = find_registration(addr, length, dev, access);
  {
    std::lock_guard<std::mutex> g(stats_mu_);
    if (reg != nullptr)
      ++stats_.registrations_reused;
    else
      ++stats_.registrations_created;
  }
  if (reg == nullptr) {
    auto owned = std::make_shared<Registration>();
    owned->base = addr;
    owned->length = length;
    owned->device = dev;
    owned->access = access;

    /* Registered with every provider, because which path a peer will be
     * reached over is not known here. A provider that refuses this particular
     * memory -- IPC cannot export what the caller allocated on the host --
     * contributes no key, and a peer arriving over that provider will find
     * the region has none, which is the truthful answer. The registration
     * fails only when no provider took it at all. */
    std::vector<std::pair<TransportProviderPtr, uint64_t>> held;
    Status first_error = Status::kOk;
    for (auto const& prov : providers_) {
      uint64_t lkey = 0, rkey = 0;
      Status s = prov->register_region(addr, length, dev, access, &lkey, &rkey);
      if (s != Status::kOk) {
        if (first_error == Status::kOk) first_error = s;
        continue;
      }
      owned->keys.push_back(Registration::Key{prov->caps().name, lkey, rkey});
      held.emplace_back(prov, lkey);
    }
    if (owned->keys.empty())
      return first_error == Status::kOk ? Status::kUnsupported : first_error;
    owned->local_key = owned->keys.front().local_key;
    owned->remote_key = owned->keys.front().remote_key;

    /* Released through the providers once nothing references it any more --
     * another handle, or the cache.
     *
     * They are captured by shared_ptr, not by raw pointer. A region can
     * outlive the engine that created it: a caller holding a handle after
     * dropping the engine is doing nothing wrong, and a raw pointer would
     * leave this deleter calling into freed memory at some later teardown,
     * with nothing in the stack to say why. */
    reg = RegistrationPtr(owned.get(), [held, owned](Registration*) mutable {
      for (auto& h : held) h.first->deregister_region(h.second);
      held.clear();
      owned.reset();
    });
    if (cfg_.registration_cache_entries > 0) cache_registration(reg);
  }

  RegionId id = next_region_.fetch_add(1, std::memory_order_relaxed);
  auto r = std::make_shared<MemoryRegionImpl>(
      id, generation_.load(std::memory_order_acquire), addr, length, dev, mem,
      access, reg);
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
  /* Peers holding a descriptor for this region are told to stop using it.
   * Without the notice they keep submitting against memory this side has
   * taken back, and find out only when the hardware refuses -- far from here,
   * and with no way to tell why. */
  RegionInvalidateBody b;
  b.region = impl->id();
  b.generation = impl->generation();
  std::vector<uint8_t> payload;
  encode_region_invalidate(b, &payload);
  std::vector<std::shared_ptr<PeerImpl>> peers;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& kv : peers_) peers.push_back(kv.second);
  }
  for (auto& p : peers) {
    if (!p->connected()) continue;
    p->provider()->send_control(
        p->conn(), static_cast<uint16_t>(ControlType::kRegionInvalidate),
        payload);
  }

  /* The underlying registration is released when the last reference to it
   * goes -- another handle over the same range, or the cache. Releasing it
   * here would pull it out from under a handle still transferring.
   *
   * So deregistering a handle does not, on its own, mean the hardware has let
   * go: with the cache on, the registration is kept for the next caller. A
   * caller about to free or unmap the memory needs
   * release_cached_registrations() as well. */
  return Status::kOk;
}

Status EngineImpl::release_cached_registrations(uint32_t* released) {
  std::vector<RegistrationPtr> dropped;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = reg_cache_.begin(); it != reg_cache_.end();) {
      /* Only what nothing else holds. One still in use stays: releasing it
       * would pull the registration out from under a live handle. */
      if (it->use_count() == 1) {
        dropped.push_back(*it);
        it = reg_cache_.erase(it);
      } else {
        ++it;
      }
    }
  }
  if (released != nullptr) *released = static_cast<uint32_t>(dropped.size());
  /* Cleared outside the lock. Deregistration can block -- the IPC path waits
   * for the peer to confirm it unmapped -- and doing that while holding the
   * engine's lock would stop the progress that delivers the confirmation. */
  dropped.clear();
  return Status::kOk;
}

Status EngineImpl::local_metadata(std::vector<uint8_t>* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  /* Identity first, then whatever the provider needs. A peer reads the
   * identity to decide which path can reach this engine at all, before it
   * cares how to dial it. */
  out->clear();
  Identity id = local_identity();
  id.engine = engine_id_;
  encode_identity(id, out);

  /* Every provider's dialling information, each named. A peer picks the one
   * it can actually use: which transport suits depends on where the peer is,
   * and that is known on its side, not here. */
  std::vector<std::pair<std::string, std::vector<uint8_t>>> metas;
  for (auto const& prov : providers_) {
    std::vector<uint8_t> m;
    if (prov->local_metadata(&m) != Status::kOk) continue;
    metas.emplace_back(prov->caps().name, std::move(m));
  }
  if (metas.empty()) return Status::kInternal;

  auto put_u16 = [out](uint16_t v) {
    out->push_back(v & 0xff);
    out->push_back((v >> 8) & 0xff);
  };
  put_u16(static_cast<uint16_t>(metas.size()));
  for (auto const& m : metas) {
    put_u16(static_cast<uint16_t>(m.first.size()));
    out->insert(out->end(), m.first.begin(), m.first.end());
    put_u16(static_cast<uint16_t>(m.second.size()));
    out->insert(out->end(), m.second.begin(), m.second.end());
  }
  return Status::kOk;
}

namespace {

/* Whether a transport is usable for a peer in that place. IPC needs one host;
 * the same-process path needs one address space. Nothing else is filtered --
 * the network path reaches a peer wherever it is, including next door. */
bool suits(std::string const& provider, Locality locality) {
  if (provider == "ipc")
    return locality == Locality::kSameHost ||
           locality == Locality::kSameProcess;
  if (provider == "local") return locality == Locality::kSameProcess;
  return true;
}

}  // namespace

Status EngineImpl::add_peer(std::vector<uint8_t> const& metadata,
                            PeerPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;

  /* Where the peer is decides which path can reach it, so it is read before
   * dialling: a peer in this process needs no connection at all. */
  Identity peer_id;
  Locality locality = Locality::kRemote;
  std::vector<uint8_t> provider_meta = metadata;
  /* What the peer offers, by provider name. Empty for metadata from a
   * single-provider peer, which carries one unnamed blob. */
  std::vector<std::pair<std::string, std::vector<uint8_t>>> offered;
  if (decode_identity(metadata, 0, &peer_id) == Status::kOk) {
    Identity mine = local_identity();
    mine.engine = engine_id_;
    locality = locality_of(mine, peer_id);
    provider_meta.assign(metadata.begin() + kIdentityBytes, metadata.end());

    size_t at = kIdentityBytes;
    auto const u16 = [&metadata](size_t i) {
      return static_cast<uint16_t>(metadata[i] |
                                   (uint16_t(metadata[i + 1]) << 8));
    };
    if (metadata.size() >= at + 2) {
      uint16_t const count = u16(at);
      at += 2;
      bool ok = true;
      std::vector<std::pair<std::string, std::vector<uint8_t>>> parsed;
      for (uint16_t i = 0; i < count && ok; ++i) {
        if (metadata.size() < at + 2) {
          ok = false;
          break;
        }
        uint16_t const n = u16(at);
        at += 2;
        if (metadata.size() < at + n + 2u) {
          ok = false;
          break;
        }
        std::string name(reinterpret_cast<char const*>(metadata.data() + at),
                         n);
        at += n;
        uint16_t const mlen = u16(at);
        at += 2;
        if (metadata.size() < at + mlen) {
          ok = false;
          break;
        }
        parsed.emplace_back(std::move(name),
                            std::vector<uint8_t>(metadata.begin() + at,
                                                 metadata.begin() + at + mlen));
        at += mlen;
      }
      /* Accepted only if it parses exactly. A trailing byte means this is not
       * the layout it looked like, and guessing would dial the wrong
       * transport with the wrong bytes. */
      if (ok && at == metadata.size()) offered = std::move(parsed);
    }
  }

  /* In local preference order, the first transport that suits where the peer
   * is and that the peer also offers. */
  TransportProviderPtr chosen;
  for (auto const& prov : providers_) {
    std::string const name = prov->caps().name;
    if (!suits(name, locality)) continue;
    if (offered.empty()) {
      chosen = prov;
      break;
    }
    for (auto const& o : offered) {
      if (o.first == name) {
        chosen = prov;
        provider_meta = o.second;
        break;
      }
    }
    if (chosen != nullptr) break;
  }
  if (chosen == nullptr) return Status::kUnsupported;

  ProviderConnectionPtr conn;
  Status s = chosen->connect(provider_meta, &conn);
  if (s != Status::kOk) return s;

  PeerCaps caps;
  caps.provider = chosen->caps().name;
  /* Reported as observed rather than assumed, so a caller that needs to know
   * whether a transfer crosses the network can ask instead of inferring it
   * from throughput. */
  /* The path taken, not the path the peer's location would allow: a peer on
   * this host reached over the network is a network transfer, and a caller
   * asking which it got must not be told the better answer. */
  switch (locality) {
    case Locality::kSameEngine:
    case Locality::kSameProcess:
      caps.place = PeerPlace::kSameProcess;
      break;
    case Locality::kSameHost:
      caps.place = PeerPlace::kSameHost;
      break;
    case Locality::kRemote:
      caps.place = PeerPlace::kAnotherHost;
      break;
  }
  if (caps.provider == "local")
    caps.path = PathKind::kSameProcess;
  else if (caps.provider == "ipc")
    caps.path = PathKind::kIpc;
  else
    caps.path = PathKind::kRdma;
  caps.qp_count = conn->qp_count();
  if (device_ != nullptr) {
    caps.remote_max_registration_bytes = device_->caps().max_registration_bytes;
  }

  PeerId id = next_peer_.fetch_add(1, std::memory_order_relaxed);
  auto p = std::make_shared<PeerImpl>(id, std::move(conn), caps, this,
                                      std::move(chosen));
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
  return p->provider()->disconnect(p->conn_ptr());
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
                                std::string const& provider,
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
      /* The key from the transport that will carry this, not whichever came
       * first. A key minted by a NIC means nothing to a mapping, and using one
       * for the other would be accepted here and refused far away. */
      uint64_t lkey = 0;
      if (!lr->local_key_for(provider, &lkey)) return Status::kUnsupported;
      op.local_key = lkey;
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
  /* The peer's transport, carried on the work item: a deferred request must
   * go out over the same one it was admitted against. */
  TransportProvider* prov =
      p->provider != nullptr ? p->provider : providers_.front().get();
  SubmitResult sr = prov->submit(p->conn.get(), slice);
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
    e.provider = prov->caps().name;
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
    e.provider = prov->caps().name;
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
  Status s =
      build_subops(local, remote, kind, req_id, p->provider()->caps().name,
                   &ops, &held, &target_addr, &target_bytes);
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
    size_t depth = 0;
    {
      std::lock_guard<std::mutex> g(mu_);
      depth = inflight_.size();
    }
    std::lock_guard<std::mutex> g(stats_mu_);
    ++stats_.requests_accepted;
    /* Peak rather than current: sizing max_inflight_requests needs what the
     * run demanded, and the value at the end says nothing about that. */
    if (depth > stats_.peak_inflight_requests)
      stats_.peak_inflight_requests = depth;
  }

  PendingSubmit ps;
  ps.req = req;
  ps.ops = std::move(ops);
  ps.conn = p->conn_ptr();
  ps.peer = p->id();
  ps.provider = p->provider();
  req->set_provider(p->provider());
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

  Status s = p->provider()->send_control(
      p->conn(), static_cast<uint16_t>(ControlType::kNotification), body);
  if (s == Status::kOk) {
    std::lock_guard<std::mutex> g(stats_mu_);
    ++stats_.notifications_sent;
  }
  if (s != Status::kOk) {
    {
      std::lock_guard<std::mutex> g(mu_);
      notify_pending_.erase(id);
    }
    ErrorInfo e;
    e.status = s;
    e.provider = p->provider()->caps().name;
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

void EngineImpl::reap_departed_peers() {
  std::vector<std::shared_ptr<PeerImpl>> gone;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& kv : peers_) {
      auto& p = kv.second;
      if (!p->connected()) continue;
      ProviderConnection* c = p->conn();
      /* Only a provider can see its channel close, and it says so here. */
      if (c != nullptr && !c->alive()) gone.push_back(p);
    }
  }
  if (gone.empty()) return;

  for (auto& p : gone) {
    /* The epoch moves first, so nothing new is admitted onto a connection
     * that is no longer there. */
    p->bump_epoch();

    std::vector<RequestImplPtr> stranded;
    {
      std::lock_guard<std::mutex> g(mu_);
      for (auto it = inflight_.begin(); it != inflight_.end();) {
        if (it->second->connection() == p->conn_ptr()) {
          stranded.push_back(it->second);
          it = inflight_.erase(it);
        } else {
          ++it;
        }
      }
    }

    for (auto& req : stranded) {
      ErrorInfo e;
      e.status = Status::kPeerDisconnected;
      e.provider = p->provider() != nullptr ? p->provider()->caps().name : "";
      e.peer_id = p->id();
      /* A write already posted may or may not have reached the peer, and
       * after a disconnect there is no way to find out. Saying so is the
       * whole point of the flag. */
      e.may_have_modified_target = req->kind() == SubOp::Kind::kWrite;
      e.detail = "peer disconnected while the request was in flight";
      req->seal_accepted();
      req->fail(e);
      {
        std::lock_guard<std::mutex> g(mu_);
        completed_.push_back(req);
      }
      std::lock_guard<std::mutex> g(stats_mu_);
      ++stats_.requests_failed;
    }
  }
}

Status EngineImpl::progress() {
  /* One scheduling pass first, so a request that has become ready is offered
   * in this same call rather than a later one. */
  drain_pending();

  /* Before anything is polled: a connection that has gone cannot produce the
   * completions the requests on it are waiting for. */
  reap_departed_peers();

  /* Every transport in turn. A peer next door and a peer on another machine
   * are reached over different ones, and a control message left unread on
   * either is a request that never completes. */
  for (auto const& prov_ptr : providers_) {
    TransportProvider* const prov = prov_ptr.get();
    std::vector<ControlMessage> msgs;
    if (prov->poll_control(cfg_.cq_batch, &msgs) == Status::kOk) {
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
          {
            std::lock_guard<std::mutex> g(stats_mu_);
            ++stats_.ready_handoffs_received;
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
          {
            std::lock_guard<std::mutex> g(stats_mu_);
            if (queued)
              ++stats_.notifications_received;
            else
              ++stats_.notifications_dropped;
          }
          /* Acknowledged only once it is actually in the queue. Confirming a
           * message that was dropped would tell the sender something false,
           * and back-pressure depends on the sender learning the truth. */
          if (queued && m.conn != nullptr) {
            std::vector<uint8_t> ack;
            encode_u64(id, &ack);
            /* Back over the transport it arrived on. */
            prov->send_control(
                m.conn, static_cast<uint16_t>(ControlType::kNotificationAck),
                ack);
          }
        } else if (static_cast<ControlType>(m.type) ==
                   ControlType::kRegionInvalidate) {
          RegionInvalidateBody b;
          if (decode_region_invalidate(m.payload, &b) != Status::kOk) continue;
          std::shared_ptr<RemoteRegionImpl> rr;
          {
            std::lock_guard<std::mutex> g(mu_);
            auto it = remotes_.find(b.region);
            if (it != remotes_.end()) rr = it->second;
          }
          /* Only if the generations match: an id can be reused, and a notice
           * for an older incarnation must not retire a newer one. */
          if (rr != nullptr && rr->generation() == b.generation) {
            rr->invalidate();
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

  for (auto const& prov : providers_) {
    std::vector<PeerArrival> arrivals;
    if (prov->poll_peer_arrivals(cfg_.cq_batch, &arrivals) == Status::kOk &&
        !arrivals.empty()) {
      std::lock_guard<std::mutex> g(mu_);
      for (auto const& a : arrivals) {
        ready_events_.push_back(std::make_shared<ReadyEventImpl>(
            a.token, a.from, device_.get(), nullptr, 0));
      }
    }
  }

  std::vector<CompletionEvent> events;
  /* Take and handle the whole batch, from every transport. The provider
   * contract requires every event it collected; returning early drops other
   * requests' completions, and skipping a transport strands whatever is in
   * flight on it. */
  for (auto const& prov : providers_) {
    std::vector<CompletionEvent> batch;
    Status ps = prov->poll(cfg_.cq_batch, &batch);
    if (ps != Status::kOk) return ps;
    events.insert(events.end(), batch.begin(), batch.end());
  }

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
        TransportProvider* prov = req->provider() != nullptr
                                      ? req->provider()
                                      : providers_.front().get();
        if (conn != nullptr) {
          prov->send_control(conn.get(),
                             static_cast<uint16_t>(ControlType::kReadyHandoff),
                             payload);
          std::lock_guard<std::mutex> g(stats_mu_);
          ++stats_.ready_handoffs_sent;
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

std::string EngineImpl::describe() const {
  std::string engine = describe_config(cfg_);
  /* Both halves, side by side, so nothing has to be inferred about which
   * settings were actually in force. The first transport is also reported on
   * its own, under the name it has always had, so a reader looking for one
   * provider still finds it where it was. */
  std::string out = std::string("{\"engine\":") + engine +
                    ",\"provider\":" + providers_.front()->describe() +
                    ",\"providers\":[";
  for (size_t i = 0; i < providers_.size(); ++i) {
    if (i > 0) out += ",";
    out += providers_[i]->describe();
  }
  out += "]}";
  return out;
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
    s.registration_cache_size = reg_cache_.size();
  }
  /* Sub-operation and byte counts come from the providers, which are the only
   * layer that knows whether a copy happened, and are summed across them: a
   * caller asking what this engine moved means all of it, however it went. */
  for (auto const& prov : providers_) {
    ProviderStats ps = prov->stats();
    s.subops_posted += ps.subops_posted;
    s.subops_completed += ps.subops_completed;
    s.subops_failed += ps.subops_failed;
    s.payload_bytes += ps.payload_bytes;
    s.payload_bytes_copied += ps.payload_bytes_copied;
  }
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
