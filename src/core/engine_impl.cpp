// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#include "core/engine_impl.h"

#include <chrono>

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
  // 逐项处理：某一项畸形不影响其余项，调用方从返回的空指针识别失败项。
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
  return Status::kInvalidArgument;  // provider 由工厂注入，见 hux::make_engine
}

// 测试与上层工厂使用的构造入口：provider 显式注入，便于用 mock 替换。
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
    // 指针实际属于哪个设备由 backend 判定，不能凭调用方声明。
    DeviceId probed;
    MemoryKind probed_kind;
    if (device_->probe_pointer(addr, &probed, &probed_kind) == Status::kOk) {
      dev = probed;
      mem = probed_kind;
    }
    // 设备有注册上限时（寒武纪 MLU 实测约 32 MiB），超限必须如实拒绝，
    // 而不是静默改走 staging 让调用方以为自己在做直传。
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
  // 逐项返回结果，部分失败不丢失已成功项的所有权。
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
  // 第一步：阻止新提交。
  impl->retire();
  // 第二步：确认没有在途请求还引用它。有则报告仍在 drain，不强行注销。
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

    // 本地与远端分段按项配对，长度必须相等。
    if (local[i].span.length != remote[i].span.length)
      return Status::kInvalidArgument;
    if (!local[i].span.within(lr->length())) return Status::kOutOfRange;
    if (!remote[i].span.within(rr->length())) return Status::kOutOfRange;

    held->push_back(lr);
    total += local[i].span.length;

    // 按 chunk_bytes 切分。chunk 是调度与限流的粒度，与应用的 segment
    // 和后端的 wr_batch 是三件不同的事，不共用一个参数。
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

  // 提交队列有上限。满时返回 kWouldBlock——逻辑请求未被接受，
  // 没有任何网络副作用，调用方可以原样重试。
  {
    std::lock_guard<std::mutex> g(mu_);
    if (inflight_.size() >= cfg_.max_inflight_requests)
      return Status::kWouldBlock;
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

  {
    std::lock_guard<std::mutex> g(mu_);
    inflight_[req_id] = req;
  }

  req->set_state(RequestState::kInflight);
  SubmitResult sr = provider_->submit(p->conn(), ops);
  req->set_accepted_subops(sr.accepted);

  if (sr.accepted == 0 && sr.status != Status::kOk) {
    // 一个子操作都没被接受：没有网络副作用，直接失败即可。
    ErrorInfo e;
    e.status = sr.status;
    e.provider = provider_->caps().name;
    e.peer_id = p->id();
    e.provider_errno = sr.provider_errno;
    e.detail = "submit rejected";
    {
      std::lock_guard<std::mutex> g(mu_);
      inflight_.erase(req_id);
    }
    req->fail(e);
    *out = req;
    return sr.status;
  }
  if (sr.accepted < ops.size()) {
    // 部分提交失败：只回滚未被接受的部分，已接受部分继续 drain，
    // 由 poll 阶段按实际接受数聚合完成。不丢 chunk，也不重复发送。
    ErrorInfo e;
    e.status = sr.status == Status::kOk ? Status::kTransportError : sr.status;
    e.provider = provider_->caps().name;
    e.peer_id = p->id();
    e.provider_errno = sr.provider_errno;
    e.may_have_modified_target = true;
    e.detail = "partial submit";
    req->fail(e);
  }

  *out = req;
  return Status::kOk;
}

Status EngineImpl::read(Peer* peer, RegionView const& local,
                        RegionView const& remote, TransferOptions const& opts,
                        RequestPtr* out) {
  // 标量是单分段便捷入口，复用同一条提交与等待逻辑。
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
  return Status::kUnsupported;  // NTF-01，见里程碑 M4
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

Status EngineImpl::poll_ready_events(uint32_t /*max_items*/,
                                     std::vector<ReadyEventPtr>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  return Status::kOk;  // 写方向的 ready 交接属于 M1，见里程碑
}

Status EngineImpl::progress() {
  std::vector<CompletionEvent> events;
  // 整批取出、整批处理。provider 契约要求它交出全部取到的事件——
  // "遇到目标即返回"会丢掉同批里其他请求的完成。
  Status s = provider_->poll(cfg_.cq_batch, &events);
  if (s != Status::kOk) return s;

  for (auto const& ev : events) {
    RequestImplPtr req;
    {
      std::lock_guard<std::mutex> g(mu_);
      auto it = inflight_.find(ev.request);
      if (it == inflight_.end()) continue;  // 已终态或已被取走
      req = it->second;
    }
    bool last = req->on_subop_complete(ev);
    if (!last) continue;

    if (req->cancel_requested()) {
      req->finish_cancelled();
    } else if (!req->error().ok()) {
      req->fail(req->error());
    } else {
      // 所有已接受的子操作都已完成 → transfer_complete。
      // 目标可见性由 DeviceBackend 负责，随后才是 target_ready。
      req->mark_stage(Stage::kTransferComplete);
      if (device_ != nullptr && req->kind() == SubOp::Kind::kRead) {
        // GPU 显存直写本身不建立与消费 kernel 的执行顺序，这一步不能省。
        req->mark_stage(Stage::kTargetReady);
      }
      req->finish_success();
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
      // 超时则保留必要资源并返回未完成状态。
      // 绝不销毁仍可能被 DMA 访问的对象。
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
