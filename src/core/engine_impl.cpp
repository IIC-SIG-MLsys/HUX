/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "core/engine_impl.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

#include "control/control_message.h"
#include "control/identity.h"
#include "core/ready_event_impl.h"
#include "hux/device.h"

namespace hux {

// ---------------- PeerImpl ----------------

PeerImpl::PeerImpl(PeerId id, Identity remote, std::vector<Lane> lanes,
                   PeerCaps caps, EngineImpl* engine)
    : id_(id),
      remote_identity_(remote),
      lanes_(std::move(lanes)),
      caps_(caps),
      engine_(engine) {}

Status PeerImpl::import_region(std::vector<uint8_t> const& descriptor,
                               RemoteRegionPtr* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  RegionDescriptor d;
  Status s = decode_descriptor(descriptor, &d);
  if (s != Status::kOk) return s;

  /* Exported by this peer, and not by something else that happens to number
   * its regions the same way. A peer that restarted is a different engine
   * and its predecessor's descriptors are refused here, where the reason is
   * "this is not yours" -- rather than several calls later, as a key the
   * hardware does not recognise.
   *
   * A peer dialled from a bare provider blob has no identity, because that
   * blob does not carry one: it is how the manual tools point a client at an
   * address typed on the command line. There is nothing to compare against,
   * so the check does not apply, and saying that here is better than letting
   * an all-zero identity fail every import. Anything that went through
   * `local_metadata` does carry an identity and is checked. */
  bool const peer_identified = remote_identity_.host != 0 ||
                               remote_identity_.process != 0 ||
                               remote_identity_.engine != 0;
  if (peer_identified && (d.origin.host != remote_identity_.host ||
                          d.origin.process != remote_identity_.process ||
                          d.origin.engine != remote_identity_.engine))
    return Status::kStaleGeneration;

  /* The peer exported a key per transport, and the whole list is kept: a
   * peer reached over more than one adapter has a key per adapter, and a
   * write carried by one cannot use another's -- an rkey belongs to the
   * protection domain that issued it, and the lane is not known until the
   * transfer is split.
   *
   * What is settled here is that at least one lane can use this region. A
   * region exported only for a path this peer is not reached over is refused
   * where the reason is visible, rather than accepted and rejected by
   * hardware somewhere with no context. */
  if (!d.provider_keys.empty()) {
    bool usable = false;
    for (auto const& lane : lanes_) {
      if (lane.provider == nullptr) continue;
      std::string const want = lane.provider->caps().name;
      for (auto const& k : d.provider_keys) {
        if (k.provider != want) continue;
        /* The first lane's key stays in the unnamed field, so anything that
         * reads it without asking for a lane gets the primary one. */
        if (&lane == &lanes_.front()) d.remote_key = k.remote_key;
        usable = true;
        break;
      }
    }
    if (!usable) return Status::kUnsupported;
  }

  auto r = std::make_shared<RemoteRegionImpl>(d);
  {
    std::lock_guard<std::mutex> g(engine_->mu_);
    engine_->remotes_[{id_, d.region}] = r;
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
  std::vector<RegistrationPtr> evicted;
  {
    std::lock_guard<std::mutex> g(mu_);
    reg_cache_.push_back(std::move(reg));
    /* Over the bound, drop entries nothing else is holding. One still in use
     * is skipped rather than evicted: releasing it would pull the
     * registration out from under a handle that may still be transferring. */
    while (reg_cache_.size() > cfg_.registration_cache_entries) {
      bool dropped = false;
      for (auto it = reg_cache_.begin(); it != reg_cache_.end(); ++it) {
        if (it->use_count() == 1) {
          evicted.push_back(std::move(*it));
          reg_cache_.erase(it);
          dropped = true;
          break;
        }
      }
      if (!dropped) break; /* everything is in use; the bound gives way */
    }
  }
  /* Released here, outside the lock, for the same reason as in
   * release_cached_registrations(): deregistration can block -- the IPC path
   * waits for the peer to confirm it unmapped -- and erasing the last
   * reference under the lock did it with every submitter and the progress
   * thread waiting on that lock. */
  evicted.clear();
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
  Identity origin = local_identity();
  origin.engine = engine_id_;
  auto r = std::make_shared<MemoryRegionImpl>(
      origin, id, generation_.load(std::memory_order_acquire), addr, length,
      dev, mem, access, reg);
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
  /* Step two: refuse while a request in flight uses this handle. Only one
   * that does: refusing while anything at all was in flight meant a region
   * could not be taken back from an engine that never went idle, and a
   * caller that did not retry left it retired in regions_ for good. */
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto const& kv : inflight_)
      for (auto const& r : kv.second->held_regions())
        if (r.get() == impl.get()) return Status::kWouldBlock;
    regions_.erase(impl->id());
  }
  /* Peers holding a descriptor for this region are told to stop using it.
   * Without the notice they keep submitting against memory this side has
   * taken back, and find out only when the hardware refuses -- far from here,
   * and with no way to tell why. */
  RegionInvalidateBody b;
  b.region = impl->id();
  b.generation = impl->generation();
  b.has_origin = true;
  b.origin = impl->origin();
  std::vector<uint8_t> payload;
  encode_region_invalidate(b, &payload);
  uint16_t const type = static_cast<uint16_t>(ControlType::kRegionInvalidate);

  /* To every connection each transport holds, not only to the peers this
   * engine added: a peer that dialled this engine has no Peer here, and it
   * is the one most likely to have imported the region. Sent only to Peers,
   * the notice reached nobody but this engine talking to itself.
   *
   * Counted by whether the transport took it, which is not the same as the
   * peer having read it: a control message it accepts may still be queued
   * for a peer that is not reading, and the provider reports what is still
   * owed. The local handle goes either way, so a notice that was refused
   * leaves the peer holding a descriptor for memory that is no longer there,
   * and this counter is the only place that shows. */
  uint64_t sent = 0, refused = 0;
  std::vector<TransportProvider*> one_by_one;
  for (auto const& prov : providers_) {
    uint32_t s = 0, r = 0;
    if (prov->broadcast_control(type, payload, &s, &r) == Status::kOk) {
      sent += s;
      refused += r;
    } else {
      one_by_one.push_back(prov.get());
    }
  }
  /* A transport that cannot reach all of its connections still reaches the
   * peers this engine dialled over it. */
  if (!one_by_one.empty()) {
    std::vector<std::shared_ptr<PeerImpl>> peers;
    {
      std::lock_guard<std::mutex> g(mu_);
      for (auto& kv : peers_) peers.push_back(kv.second);
    }
    for (auto& p : peers) {
      if (!p->connected()) continue;
      if (std::find(one_by_one.begin(), one_by_one.end(), p->provider()) ==
          one_by_one.end())
        continue;
      if (p->provider()->send_control(p->conn(), type, payload) == Status::kOk)
        ++sent;
      else
        ++refused;
    }
  }
  {
    std::lock_guard<std::mutex> g(stats_mu_);
    stats_.region_invalidates_sent += sent;
    stats_.region_invalidates_failed += refused;
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
  /* Named so the reader can identify this layout instead of inferring it.
   * See Engine::kMetadataMagic. */
  put_u16(kMetadataMagic & 0xffff);
  put_u16((kMetadataMagic >> 16) & 0xffff);
  put_u16(kMetadataMajor);
  put_u16(kMetadataMinor);
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
  /* What the peer offers, by provider name. Empty when the blob is one
   * provider's dialling information rather than an engine's metadata, which
   * is how the manual tools point a client at an address by hand. */
  std::vector<std::pair<std::string, std::vector<uint8_t>>> offered;

  auto const u16_at = [&metadata](size_t i) {
    return static_cast<uint16_t>(metadata[i] |
                                 (uint16_t(metadata[i + 1]) << 8));
  };
  /* Identity, magic, major, minor: the shortest thing that can be an
   * engine's metadata. Anything shorter, or without the magic, is taken as a
   * provider blob. */
  bool const is_envelope =
      metadata.size() >= kIdentityBytes + 8 &&
      (uint32_t(u16_at(kIdentityBytes)) |
       (uint32_t(u16_at(kIdentityBytes + 2)) << 16)) == kMetadataMagic;
  /* Refused rather than read as far as it goes. A major change moves fields
   * this end would otherwise read out of a neighbour's bytes. */
  if (is_envelope && u16_at(kIdentityBytes + 4) != kMetadataMajor)
    return Status::kUnsupported;

  if (is_envelope && decode_identity(metadata, 0, &peer_id) == Status::kOk) {
    Identity mine = local_identity();
    mine.engine = engine_id_;
    locality = locality_of(mine, peer_id);
    provider_meta.assign(metadata.begin() + kIdentityBytes, metadata.end());

    /* Past the identity, the magic and the version. The count is checked
     * for like every field after it: the envelope test above only proves
     * the bytes before it are there. */
    size_t at = kIdentityBytes + 8;
    auto const& u16 = u16_at;
    if (metadata.size() < at + 2) return Status::kInvalidArgument;
    {
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
      /* The magic says this is the layout, so a blob that does not parse
       * exactly is malformed rather than something else that happens to
       * start the same way. */
      if (!ok || at != metadata.size()) return Status::kInvalidArgument;
      offered = std::move(parsed);
    }
  }

  /* In local preference order, the first transport that suits where the peer
   * is and that the peer also offers -- and then every sibling of it, so a
   * transfer can be split across them.
   *
   * Siblings are transports of one family: "rdma" and "rdma#1" are the same
   * thing on two adapters, while "ipc" is a different path entirely.
   * Splitting a transfer between an adapter and shared memory would be
   * splitting it between two different answers to where the peer is. */
  auto const family = [](std::string const& n) {
    size_t const h = n.find('#');
    return h == std::string::npos ? n : n.substr(0, h);
  };

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
  std::string const chosen_family = family(chosen->caps().name);

  std::vector<PeerImpl::Lane> lanes;
  {
    ProviderConnectionPtr conn;
    Status const s = chosen->connect(provider_meta, &conn);
    if (s != Status::kOk) return s;
    lanes.push_back(
        {chosen, std::move(conn), chosen->caps().relative_capacity});
  }

  /* The rest of the family, where the peer offers them too. A lane that will
   * not connect is left out rather than failing the peer: one adapter being
   * unreachable is a reason to use the others, not to give up. */
  for (auto const& prov : providers_) {
    if (prov == chosen) continue;
    std::string const name = prov->caps().name;
    if (!suits(name, locality) || family(name) != chosen_family) continue;
    for (auto const& o : offered) {
      if (o.first != name) continue;
      ProviderConnectionPtr lane_conn;
      if (prov->connect(o.second, &lane_conn) == Status::kOk)
        lanes.push_back(
            {prov, std::move(lane_conn), prov->caps().relative_capacity});
      break;
    }
  }

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
  else if (chosen_family == "ucx")
    caps.path = PathKind::kUcx;
  else
    caps.path = PathKind::kRdma;
  /* Across every lane, since that is how many queue pairs this peer is
   * actually reached over. One adapter's count would understate it. */
  caps.qp_count = 0;
  for (auto const& l : lanes)
    if (l.conn != nullptr) caps.qp_count += l.conn->qp_count();
  caps.lane_count = static_cast<uint32_t>(lanes.size());
  if (device_ != nullptr) {
    caps.remote_max_registration_bytes = device_->caps().max_registration_bytes;
  }

  PeerId id = next_peer_.fetch_add(1, std::memory_order_relaxed);
  auto p =
      std::make_shared<PeerImpl>(id, peer_id, std::move(lanes), caps, this);
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
  /* No acknowledgement is read for a peer that is no longer here. */
  ErrorInfo ne;
  ne.status = Status::kPeerDisconnected;
  ne.peer_id = p->id();
  ne.detail =
      "the peer was removed before acknowledging; whether it arrived "
      "is not known";
  end_notifications(p->id(), false, ne);
  /* Every lane. Leaving one connected would hold the peer's memory
   * registered on that adapter and keep a channel open that nothing will
   * ever read again. The first failure is reported and the rest are still
   * closed, because half a disconnect is worse than either outcome. */
  Status first = Status::kOk;
  for (auto const& l : p->lanes()) {
    if (l.provider == nullptr || l.conn == nullptr) continue;
    Status const s = l.provider->disconnect(l.conn);
    if (s != Status::kOk && first == Status::kOk) first = s;
  }
  return first;
}

std::shared_ptr<MemoryRegionImpl> EngineImpl::find_region(RegionId id) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = regions_.find(id);
  return it == regions_.end() ? nullptr : it->second;
}

PeerId EngineImpl::peer_for_conn(ProviderConnection* conn) const {
  if (conn == nullptr) return 0;
  std::lock_guard<std::mutex> g(mu_);
  /* Any lane, not only the first: each is a connection of this peer's. */
  for (auto const& kv : peers_)
    for (auto const& l : kv.second->lanes())
      if (l.conn.get() == conn) return kv.second->id();
  /* A connection this engine accepted without an application ever adding a
   * Peer for it. Nothing to scope against, and saying so is better than
   * guessing. */
  return 0;
}

std::shared_ptr<RemoteRegionImpl> EngineImpl::find_remote(PeerId peer,
                                                          RegionId id) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = remotes_.find({peer, id});
  return it == remotes_.end() ? nullptr : it->second;
}

Status EngineImpl::build_subops(PeerId peer,
                                std::vector<RegionView> const& local,
                                std::vector<RegionView> const& remote,
                                SubOp::Kind kind, RequestId req,
                                std::vector<PeerImpl::Lane> const& lanes,
                                std::vector<std::vector<SubOp>>* out,
                                std::vector<MemoryRegionPtr>* held,
                                void** target_addr, uint64_t* target_bytes) {
  if (lanes.empty()) return Status::kNotFound;
  out->assign(lanes.size(), {});
  /* Bytes already given to each lane, so the next chunk can go where it
   * costs least in proportion to what that lane can carry. Whole chunks are
   * assigned rather than fractions of one: a chunk is already the scheduling
   * unit, and splitting below it would trade balance for round trips. */
  std::vector<double> assigned(lanes.size(), 0.0);
  /* No chunk longer than any lane can carry in one operation. The engine
   * used to split by chunk_bytes alone, whatever a transport said its limit
   * was. */
  uint64_t chunk = cfg_.chunk_bytes;
  for (auto const& l : lanes) {
    uint64_t const most = l.provider->caps().max_segment_bytes;
    if (most > 0 && most < chunk) chunk = most;
  }

  uint64_t sub_id = 0;
  uint64_t total = 0;
  for (size_t i = 0; i < local.size(); ++i) {
    auto lr = find_region(local[i].region);
    if (lr == nullptr) return Status::kNotFound;
    if (lr->retired()) return Status::kStaleGeneration;
    auto rr = find_remote(peer, remote[i].region);
    if (rr == nullptr) return Status::kNotFound;
    if (!rr->valid()) return Status::kStaleGeneration;
    /* What its owner exported it for. An adapter enforces that at the far
     * end, as a remote access error that moves the queue pair to error and
     * fails every request on the connection with it; the local and IPC
     * paths not at all, and wrote into a region exported for reading. So it
     * is refused here, before anything is posted. */
    AccessFlags const needed = kind == SubOp::Kind::kWrite
                                   ? AccessFlags::kRemoteWrite
                                   : AccessFlags::kRemoteRead;
    if (!has_flag(rr->access(), needed)) return Status::kInvalidArgument;

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
      if (n > chunk) n = chunk;
      /* Whichever lane this chunk costs least, in proportion to what that
       * lane carries. With one lane this picks it every time and nothing
       * about the result changes. */
      size_t lane = 0;
      double best = 0;
      bool have = false;
      for (size_t k = 0; k < lanes.size(); ++k) {
        double const w = lanes[k].weight > 0 ? lanes[k].weight : 1e-9;
        double const cost = (assigned[k] + static_cast<double>(n)) / w;
        if (!have || cost < best) {
          best = cost;
          lane = k;
          have = true;
        }
      }

      std::string const& lane_name = lanes[lane].provider->caps().name;
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
      if (!lr->local_key_for(lane_name, &lkey)) return Status::kUnsupported;
      op.local_key = lkey;
      op.remote_addr = rr->base() + remote[i].span.offset + off;
      /* And the peer's key for that same transport, for the same reason from
       * the other end. */
      if (!rr->remote_key_for(lane_name, &op.remote_key))
        return Status::kUnsupported;
      op.length = n;
      (*out)[lane].push_back(op);
      assigned[lane] += static_cast<double>(n);
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

Status EngineImpl::dependency_state(
    std::vector<DeviceEventPtr> const& after) const {
  Status state = Status::kOk;
  for (auto const& e : after) {
    if (e == nullptr) continue;
    /* Never recorded means it captures no work at all; waiting on it would
     * order nothing, so it can never count as satisfied -- and it never
     * will, since an event is not a promise of work to come. */
    if (!e->recorded()) return Status::kInvalidArgument;
    bool complete = false;
    Status const q = e->query(&complete);
    if (q != Status::kOk) return q;
    if (!complete) state = Status::kWouldBlock;
  }
  return state;
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

  /* Another lane of this request failed, or it was cancelled, or it has
   * ended: what is left of this lane must not go out. Posting it would put
   * the NIC on a source that the request may already have told its caller
   * is safe to reuse. */
  if (p->req->abandoning()) {
    p->ops.clear();
    part_done(p->req);
    return true;
  }

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

  if (sr.accepted == slice.size()) {
    if (!p->ops.empty()) return false; /* the rest of this lane, next turn */
    part_done(p->req);
    return true;
  }

  /* Submission has stopped for a reason that is not back-pressure, so no
   * more of this lane will be posted -- and none of the request's other
   * lanes either, which see the error and give up what they have left. */
  uint32_t const outstanding = p->req->accepted_subops();
  ErrorInfo e;
  e.status = sr.status == Status::kOk ? Status::kTransportError : sr.status;
  e.provider = prov->caps().name;
  e.peer_id = p->peer;
  e.provider_errno = sr.provider_errno;
  e.may_have_modified_target = outstanding > 0;
  e.detail = outstanding == 0 ? "submit rejected" : "partial submit";
  p->req->note_error(e);
  p->req->set_state(RequestState::kDraining);
  p->ops.clear();
  part_done(p->req);
  return true;
}

void EngineImpl::part_done(RequestImplPtr const& req) {
  /* Only the last lane to finish decides, and only for a request that
   * cannot succeed. Before the last, another lane may still post; and a
   * request with nothing wrong completes through its sub-operations.
   *
   * Deciding any earlier is how a request split across adapters used to be
   * failed -- FailedSafe published, the entry dropped from inflight_ --
   * while a second lane was still to be posted, and then was posted: the
   * NIC read a source its caller had been told was free, and the completions
   * that followed were thrown away for belonging to nothing. */
  if (!req->part_finished() || !req->abandoning()) return;
  /* Outstanding sub-operations end the request through the completion path,
   * which measures against what was accepted now that it is sealed. With
   * none, no completion is coming, so it ends here. */
  if (req->seal_if_idle()) end_abandoned(req);
}

void EngineImpl::end_abandoned(RequestImplPtr const& req) {
  /* Ended by whoever takes it out of inflight_, here as on every other
   * path. A submitter giving up and the peer reaper could both reach the
   * same request, and each counted it and queued it for poll_completions --
   * so a caller got it twice, and freed its context twice. */
  if (!claim(req->id())) return;
  /* Nothing of it is in flight, so local DMA has stopped -- or never
   * started -- and FailedSafe or CancelledSafe is true. Counted before it
   * is released, as every other ending is. */
  bool const cancelled = req->cancel_requested();
  {
    std::lock_guard<std::mutex> g(stats_mu_);
    if (cancelled)
      ++stats_.requests_cancelled;
    else
      ++stats_.requests_failed;
  }
  if (cancelled)
    req->finish_cancelled();
  else
    req->fail(req->error());
  publish(req);
}

bool EngineImpl::claim(RequestId id) {
  std::lock_guard<std::mutex> g(mu_);
  return inflight_.erase(id) > 0;
}

RequestImplPtr EngineImpl::take_notification(uint64_t id) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = notify_pending_.find(id);
  if (it == notify_pending_.end()) return nullptr;
  RequestImplPtr req = std::move(it->second);
  notify_pending_.erase(it);
  return req;
}

void EngineImpl::end_notifications(PeerId peer, bool only_cancelled,
                                   ErrorInfo const& e) {
  /* A notification ends on its acknowledgement, and there are ways for that
   * never to come: the peer goes, or is removed, or the caller stops
   * waiting. Each of them left the request -- and the entry holding it --
   * for good. */
  std::vector<RequestImplPtr> ended;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = notify_pending_.begin(); it != notify_pending_.end();) {
      RequestImplPtr const& r = it->second;
      bool const match = only_cancelled ? r->cancel_requested()
                                        : (peer == 0 || r->peer() == peer);
      if (match) {
        ended.push_back(r);
        it = notify_pending_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& r : ended) {
    if (e.ok() || r->cancel_requested())
      r->finish_cancelled();
    else
      r->fail(e);
    publish(r);
  }
}

void EngineImpl::publish(RequestImplPtr const& req) {
  /* Queued for poll_completions only once it has ended: queued first, a
   * poller could be handed a request that was not yet done. */
  RequestPtr evicted; /* released after mu_, see keep_completed_locked */
  std::lock_guard<std::mutex> g(mu_);
  evicted = keep_completed_locked(req);
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
    /* A request that has failed or been cancelled gives up what it has not
     * posted without waiting for its dependencies: they may never be met,
     * and it is not going to run. Left waiting, a cancelled request stayed
     * in the queue until its producer finished, and then ran in full. */
    if (!p.req->abandoning()) {
      Status const d = dependency_state(p.after);
      if (d == Status::kWouldBlock) {
        still_waiting.push_back(std::move(p));
        continue;
      }
      if (d != Status::kOk) {
        /* One that can no longer be met ends the request, failed: nothing
         * of it was posted, so that is safe, where waiting on was for
         * ever. */
        ErrorInfo e;
        e.status = d;
        p.req->note_error(e);
      }
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
  /* Not implemented. Accepted, the request went ahead and the notification
   * it asked for was never sent, with nothing to say so. */
  if (opts.notify) return Status::kUnsupported;

  auto* p = static_cast<PeerImpl*>(peer);
  if (!p->connected()) return Status::kPeerDisconnected;

  /* A dependency that can never complete is refused before anything is
   * admitted: an event never recorded captures no work, and one whose query
   * fails will not start succeeding. Accepted, either left the request
   * waiting on it for ever. */
  {
    Status const dep = dependency_state(opts.after);
    if (dep != Status::kOk && dep != Status::kWouldBlock) return dep;
  }

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
  std::vector<std::vector<SubOp>> per_lane;
  std::vector<MemoryRegionPtr> held;
  void* target_addr = nullptr;
  uint64_t target_bytes = 0;
  Status s = build_subops(p->id(), local, remote, kind, req_id, p->lanes(),
                          &per_lane, &held, &target_addr, &target_bytes);
  if (s != Status::kOk) return s;

  size_t total_ops = 0;
  for (auto const& v : per_lane) total_ops += v.size();
  /* Moving nothing is refused. A request without a single sub-operation has
   * nothing that could ever end it: it stayed in flight for good, a wait on
   * it never returned, and every deregistration after it was turned away as
   * busy. Nothing has been admitted yet, so refusing here leaves no trace. */
  if (total_ops == 0) return Status::kInvalidArgument;

  auto req = std::make_shared<RequestImpl>(
      req_id, kind, static_cast<uint32_t>(total_ops), opts.context);
  for (auto& r : held) req->hold_region(std::move(r));
  req->hold_connection(p->conn_ptr());
  req->set_peer(p->id());
  req->set_device_backend(device_.get());
  req->set_target(target_addr, target_bytes);
  if (kind == SubOp::Kind::kWrite) {
    /* Every segment's own range, so the handoff names the bytes written and
     * no others. */
    for (auto const& seg : remote) {
      if (seg.span.length == 0) continue;
      auto rr = find_remote(p->id(), seg.region);
      if (rr != nullptr)
        req->add_remote_target(seg.region, rr->generation(), seg.span);
    }
  }

  {
    std::lock_guard<std::mutex> g(mu_);
    /* Checked again where close() looks: checked only on the way in, a
     * transfer submitted while the engine closed could be admitted after
     * close() had found nothing in flight and returned -- and then posted,
     * into memory its caller was free to release. */
    if (closed_.load(std::memory_order_acquire))
      return Status::kInvalidArgument;
    /* And the regions, for the same reason: checked only while building,
     * one deregistered after that saw nothing in flight and returned, and
     * the transfer was then posted against it. Retiring comes before
     * deregistration looks here, so one or the other sees the other. */
    for (auto const& r : req->held_regions())
      if (static_cast<MemoryRegionImpl const*>(r.get())->retired())
        return Status::kStaleGeneration;
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

  /* One piece of pending work per lane. The request counts sub-operations
   * rather than connections, so it completes when every lane's share has,
   * and nothing below here has to know a transfer was split. */
  std::vector<PendingSubmit> parts;
  for (size_t k = 0; k < per_lane.size(); ++k) {
    if (per_lane[k].empty()) continue;
    PendingSubmit ps;
    ps.req = req;
    ps.ops = std::move(per_lane[k]);
    ps.conn = p->lanes()[k].conn;
    req->hold_lane(ps.conn);
    ps.peer = p->id();
    ps.provider = p->lanes()[k].provider.get();
    ps.after = opts.after;
    parts.push_back(std::move(ps));
  }
  /* Counted before any is posted, so the first lane to fail cannot end the
   * request while another is still to go. */
  req->set_parts(static_cast<uint32_t>(parts.size()));
  /* The first lane, for what belongs to a single connection: a ready handoff
   * travels back over one channel, not over whichever carried a chunk. */
  req->set_provider(p->provider());

  Status const dep =
      opts.after.empty() ? Status::kOk : dependency_state(opts.after);
  if (dep == Status::kWouldBlock) {
    /* The request is accepted and handed back now; only its submission waits,
     * so the calling thread never blocks on the device. */
    req->set_state(RequestState::kWaitDependency);
    {
      std::lock_guard<std::mutex> g(mu_);
      for (auto& ps : parts) pending_.push_back(std::move(ps));
    }
    *out = req;
    return Status::kOk;
  }
  if (dep != Status::kOk) {
    /* Failed since it was checked on the way in. Posting below then gives
     * the request up, with nothing sent. */
    ErrorInfo e;
    e.status = dep;
    req->note_error(e);
  }

  for (auto& ps : parts) {
    if (!post_ops(&ps, cfg_.scheduler_quantum_bytes)) {
      /* Not all of it fit in one turn; the rest waits for a later pass. */
      std::lock_guard<std::mutex> g(mu_);
      pending_.push_back(std::move(ps));
    }
  }
  /* Admitted, so it ends through the request like any other, and the call
   * says kOk however soon it failed. Returning the error as well reported
   * one failure twice -- here, and again in poll_completions, where it had
   * already been queued -- and a caller that released its context on both
   * released it twice. With a lane still waiting its turn it was worse: the
   * error came back now and the ending later. */
  *out = req;
  return Status::kOk;
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
  /* So a peer that goes, or is removed, can take its unacknowledged
   * notifications with it rather than leave them waiting. */
  req->set_peer(p->id());

  std::vector<uint8_t> body(kNotificationHeaderBytes + payload.size());
  std::vector<uint8_t> idbuf;
  encode_u64(id, &idbuf);
  std::copy(idbuf.begin(), idbuf.end(), body.begin());
  std::copy(payload.begin(), payload.end(),
            body.begin() + kNotificationHeaderBytes);

  {
    std::lock_guard<std::mutex> g(mu_);
    /* Checked again where close() looks for what is left. Checked only on
     * the way in, one sent while the engine closed was recorded after close()
     * had ended the rest, and was never ended at all. */
    if (closed_.load(std::memory_order_acquire))
      return Status::kInvalidArgument;
    notify_pending_[id] = req;
  }

  Status s = p->provider()->send_control(
      p->conn(), static_cast<uint16_t>(ControlType::kNotification), body);
  if (s == Status::kOk) {
    std::lock_guard<std::mutex> g(stats_mu_);
    ++stats_.notifications_sent;
  } else if (take_notification(id) != nullptr) {
    /* Unless it has ended already: once recorded, a departing peer can end
     * it. */
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

RequestPtr EngineImpl::keep_completed_locked(RequestPtr req) {
  /* Kept only for poll_completions, and bounded, because an application
   * that waits on its requests never collects them here: unbounded, this
   * queue held every request such an application ever made. */
  if (cfg_.completion_queue_depth == 0) {
    completions_dropped_.fetch_add(1, std::memory_order_relaxed);
    return req;
  }
  completed_.push_back(std::move(req));
  if (completed_.size() <= cfg_.completion_queue_depth) return nullptr;
  RequestPtr oldest = std::move(completed_.front());
  completed_.pop_front();
  completions_dropped_.fetch_add(1, std::memory_order_relaxed);
  return oldest;
}

void EngineImpl::keep_ready_locked(ReadyEventPtr ev) {
  /* The same bound for a receiver that never asks for its ready events --
   * but past it the new event is refused, not the oldest. A consumer takes
   * these in order and is waiting on the oldest first; dropping those would
   * leave it waiting for a signal that was thrown away, while refusing new
   * ones leaves everything before the gap intact. Notifications are bounded
   * the same way. */
  if (ready_events_.size() >= cfg_.ready_queue_depth) {
    ready_events_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ready_events_.push_back(std::move(ev));
}

Status EngineImpl::poll_completions(uint32_t max_items,
                                    std::vector<RequestPtr>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  /* What finished is handed out even when a transport's poll failed: the
   * error is returned, but returning it instead left every finished request
   * in the queue for as long as the failure lasted. */
  Status const s =
      cfg_.progress == ProgressMode::kExplicit ? progress() : Status::kOk;
  std::lock_guard<std::mutex> g(mu_);
  while (!completed_.empty() && out->size() < max_items) {
    out->push_back(std::move(completed_.front()));
    completed_.pop_front();
  }
  return s;
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
      /* Only a provider can see its channel close, and it says so here.
       * Any lane, not the first: a transfer split across them cannot
       * complete on the strength of the ones still up. */
      if (p->any_lane_gone()) gone.push_back(p);
    }
  }
  if (gone.empty()) return;

  for (auto& p : gone) {
    /* The epoch moves first, so nothing new is admitted onto a connection
     * that is no longer there. */
    p->bump_epoch();

    /* Taking a request out of inflight_ is what claims it: whatever else was
     * about to end one of these finds it gone and leaves it alone. */
    std::vector<RequestImplPtr> stranded;
    {
      std::lock_guard<std::mutex> g(mu_);
      for (auto it = inflight_.begin(); it != inflight_.end();) {
        /* By peer, not by connection: a split transfer sits on several of
         * this peer's connections and matching one of them would strand only
         * part of it. */
        if (it->second->peer() == p->id()) {
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
      /* Counted before it is released, as every other ending is. */
      {
        std::lock_guard<std::mutex> g(stats_mu_);
        ++stats_.requests_failed;
      }
      req->fail(e);
      publish(req);
    }

    ErrorInfo ne;
    ne.status = Status::kPeerDisconnected;
    ne.peer_id = p->id();
    ne.detail =
        "the peer went before acknowledging; whether it arrived is "
        "not known";
    end_notifications(p->id(), false, ne);
  }
}

Status EngineImpl::progress() {
  std::lock_guard<std::mutex> turn(progress_mu_);
  /* One scheduling pass first, so a request that has become ready is offered
   * in this same call rather than a later one. */
  drain_pending();

  /* Before anything is polled: a connection that has gone cannot produce the
   * completions the requests on it are waiting for. */
  reap_departed_peers();
  /* A cancelled notification has nothing to drain: its bytes have gone, and
   * only the wait for the acknowledgement is left to stop. */
  end_notifications(0, true, ErrorInfo{});

  /* Every transport in turn. A peer next door and a peer on another machine
   * are reached over different ones, and a control message left unread on
   * either is a request that never completes. */
  for (auto const& prov_ptr : providers_) {
    TransportProvider* const prov = prov_ptr.get();
    std::vector<ControlMessage> msgs;
    if (prov->poll_control(cfg_.cq_batch, &msgs) == Status::kOk) {
      for (auto& m : msgs) {
        /* Which peer this came from, in the engine's own numbering. A
         * provider cannot fill this in: it knows connections and its own
         * notion of identity, not the ids handed to the application. Without
         * it a control message can only be matched by what it names, and
         * every peer numbers its regions from one. */
        /* Always this engine's answer, 0 when it has none. Keeping a
         * provider's own value when there was no match handed out another
         * numbering -- a process id -- as if it were a PeerId. */
        m.peer = peer_for_conn(m.conn);
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
          keep_ready_locked(std::make_shared<ReadyEventImpl>(
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
            /* Back over the transport it arrived on. Counted when it
             * fails, because the comment above is the whole point: the
             * sender's notify() completes on this acknowledgement, and an
             * acknowledgement that never left is indistinguishable here
             * from one that did unless somebody looks. */
            if (prov->send_control(
                    m.conn,
                    static_cast<uint16_t>(ControlType::kNotificationAck),
                    ack) != Status::kOk) {
              std::lock_guard<std::mutex> g(stats_mu_);
              ++stats_.notification_acks_failed;
            }
          } else if (!queued && m.conn != nullptr) {
            /* And a dropped one is refused, so the sender fails it instead
             * of waiting for an acknowledgement that is never coming. */
            std::vector<uint8_t> refused;
            encode_u64(id, &refused);
            prov->send_control(
                m.conn,
                static_cast<uint16_t>(ControlType::kNotificationRefused),
                refused);
          }
        } else if (static_cast<ControlType>(m.type) ==
                   ControlType::kRegionInvalidate) {
          RegionInvalidateBody b;
          if (decode_region_invalidate(m.payload, &b) != Status::kOk) continue;
          /* Scoped to the engine that exported it: another peer's region
           * can share the id, and retiring the wrong one would silently stop
           * a healthy path. By its identity when the notice carries one,
           * whichever connection it came in on and whichever Peer imported
           * the region; by the connection's peer otherwise. */
          std::vector<std::shared_ptr<RemoteRegionImpl>> named;
          {
            std::lock_guard<std::mutex> g(mu_);
            if (b.has_origin) {
              for (auto const& kv : remotes_)
                if (kv.first.second == b.region &&
                    kv.second->origin() == b.origin)
                  named.push_back(kv.second);
            } else if (m.peer != 0) {
              auto it = remotes_.find({m.peer, b.region});
              if (it != remotes_.end()) named.push_back(it->second);
            }
          }
          /* Only if the generations match: an id can be reused, and a notice
           * for an older incarnation must not retire a newer one. */
          for (auto const& rr : named)
            if (rr->generation() == b.generation) rr->invalidate();
        } else if (static_cast<ControlType>(m.type) ==
                   ControlType::kNotificationAck) {
          uint64_t id = 0;
          if (decode_u64(m.payload, &id) != Status::kOk) continue;
          RequestImplPtr req = take_notification(id);
          if (req != nullptr) {
            req->mark_stage(Stage::kTransferComplete);
            req->finish_success();
            publish(req);
          }
        } else if (static_cast<ControlType>(m.type) ==
                   ControlType::kNotificationRefused) {
          uint64_t id = 0;
          if (decode_u64(m.payload, &id) != Status::kOk) continue;
          RequestImplPtr req = take_notification(id);
          if (req != nullptr) {
            ErrorInfo e;
            e.status = Status::kResourceExhausted;
            e.peer_id = m.peer;
            e.detail = "the peer's notification queue was full";
            req->fail(e);
            publish(req);
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
        keep_ready_locked(std::make_shared<ReadyEventImpl>(
            a.token, a.from, device_.get(), nullptr, 0));
      }
    }
  }

  std::vector<CompletionEvent> events;
  /* Take and handle the whole batch, from every transport. The provider
   * contract requires every event it collected; returning early drops other
   * requests' completions, and skipping a transport strands whatever is in
   * flight on it. */
  Status polled = Status::kOk;
  for (auto const& prov : providers_) {
    std::vector<CompletionEvent> batch;
    Status const ps = prov->poll(cfg_.cq_batch, &batch);
    /* Kept and reported, not returned on: returning here dropped the events
     * already taken from the transports before this one, and a lasting
     * error on one adapter starved every transport after it. */
    if (ps != Status::kOk && polled == Status::kOk) polled = ps;
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
    /* Ended by whoever takes it out of inflight_: the reaper may have taken
     * it since it was looked up. */
    if (!claim(ev.request)) continue;

    /* Counted before the request is released, in all three cases.
     *
     * Finishing a request wakes whoever is waiting on it, and that caller
     * may read the counters straight afterwards. Counting second leaves a
     * window in which every request a caller has seen finish is not yet in
     * the totals -- rare on an idle machine and about eight times in a
     * hundred under contention, which is how it reached the pipeline rather
     * than a desk. If a caller has seen it end, the counters say so. */
    if (req->cancel_requested()) {
      {
        std::lock_guard<std::mutex> g(stats_mu_);
        ++stats_.requests_cancelled;
      }
      req->finish_cancelled();
    } else if (!req->error().ok()) {
      {
        std::lock_guard<std::mutex> g(stats_mu_);
        ++stats_.requests_failed;
      }
      req->fail(req->error());
    } else {
      /* Every accepted sub-operation is done, so transfer_complete holds.
       * Device visibility comes next, and only then target_ready. */
      req->mark_stage(Stage::kTransferComplete);
      if (device_ != nullptr && req->kind() == SubOp::Kind::kRead) {
        /* A direct write to device memory orders nothing against a consuming
         * kernel on its own. */
        req->mark_stage(Stage::kTargetReady);
      }
      for (auto const& t : req->remote_targets()) {
        /* The peer knows bytes arrived from the immediate value, but not
         * which ones. This says so, and travels on the control channel so a
         * congested data path cannot delay it. One per range written. */
        ReadyHandoffBody b;
        b.request = req->id();
        b.region = t.region;
        b.generation = t.generation;
        b.offset = t.span.offset;
        b.length = t.span.length;
        std::vector<uint8_t> payload;
        encode_ready_handoff(b, &payload);
        ProviderConnectionPtr conn = req->connection();
        TransportProvider* prov = req->provider() != nullptr
                                      ? req->provider()
                                      : providers_.front().get();
        if (conn != nullptr) {
          /* Counted by whether the transport took it. Taken is not the
           * same as delivered -- it may still be queued for a peer that is
           * not reading, which the provider reports separately -- but
           * refused is certain: the request is about to succeed either way,
           * the bytes did land, and a handoff nobody took leaves the peer
           * waiting for a signal that is never coming. This counter is the
           * only place it shows. */
          Status const sent = prov->send_control(
              conn.get(), static_cast<uint16_t>(ControlType::kReadyHandoff),
              payload);
          std::lock_guard<std::mutex> g(stats_mu_);
          if (sent == Status::kOk)
            ++stats_.ready_handoffs_sent;
          else
            ++stats_.ready_handoffs_failed;
        }
      }
      {
        std::lock_guard<std::mutex> g(stats_mu_);
        ++stats_.requests_succeeded;
      }
      req->finish_success();
    }
    publish(req);
  }
  return polled;
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
  s.completions_dropped = completions_dropped_.load(std::memory_order_relaxed);
  s.ready_events_dropped =
      ready_events_dropped_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> g(mu_);
    /* Requests, not their pieces, and only those waiting on a dependency:
     * pending_ holds a piece per lane, and pieces put back for want of a
     * turn, so its size counted a split request once per lane and a large
     * one that was merely still going out as if it were waiting. */
    std::unordered_set<RequestImpl const*> waiting;
    for (auto const& ps : pending_)
      if (ps.req->state() == RequestState::kWaitDependency)
        waiting.insert(ps.req.get());
    s.requests_waiting_on_dependency = waiting.size();
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
  /* Notifications still waiting for their acknowledgement are cancelled
   * rather than waited for: they hold no memory, and with the engine stopped
   * nothing would read the acknowledgement, so a wait on one never returned.
   * One sent after this finds closed_ set where it would be recorded. */
  end_notifications(0, false, ErrorInfo{});
  stopping_.store(true, std::memory_order_release);
  if (progress_thread_.joinable()) progress_thread_.join();
  return Status::kOk;
}

}  // namespace hux
