/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "transport/ucx/ucx_provider.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

#include "control/control_message.h"

namespace hux {
namespace {

/* One in-flight operation. UCX hands back a pointer to this through the
 * request callback, which is how a completion is matched to the sub-operation
 * that caused it. */
struct UcxRequest {
  RequestId request = 0;
  uint64_t sub_id = 0;
  uint64_t bytes = 0;
  bool is_write = false;
  bool done = false;
  Status status = Status::kOk;
  UcxProvider* owner = nullptr;
  /* The endpoint a finished write still has to be flushed on. */
  UcxConnection* conn = nullptr;
};

void request_init(void* r) {
  auto* u = static_cast<UcxRequest*>(r);
  new (u) UcxRequest();
}

/* UCX initializes a request's memory only the first time it allocates it,
 * not each time it hands it out again, so a request goes back reset: freed
 * as it was, the next operation to get it would start out done, and a read
 * -- or a flush -- would be reported finished before it had begun. */
void release(void* req) {
  *static_cast<UcxRequest*>(req) = UcxRequest();
  ucp_request_free(req);
}

}  // namespace

/* A connection is one endpoint. The peer's rkeys are unpacked against it,
 * since an rkey is only meaningful for the endpoint it was unpacked on. */
class UcxConnection : public ProviderConnection {
 public:
  UcxConnection(UcxProvider* owner, ucp_ep_h ep) : owner_(owner), ep_(ep) {}
  ~UcxConnection() {
    std::lock_guard<std::mutex> w(owner_->worker_mu_);
    for (auto& kv : rkeys_) ucp_rkey_destroy(kv.second);
  }

  uint32_t qp_count() const override { return 1; }
  uint32_t submit_capacity() const override { return 1024; }

  ucp_ep_h ep() const { return ep_; }

  /* Unpacks once per remote key and keeps it: unpacking per transfer would
   * repeat work UCX does not make cheap. Caller holds the worker lock. */
  ucp_rkey_h rkey_for(uint64_t key, std::vector<uint8_t> const& blob) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = rkeys_.find(key);
    if (it != rkeys_.end()) return it->second;
    ucp_rkey_h rkey = nullptr;
    if (ucp_ep_rkey_unpack(ep_, blob.data(), &rkey) != UCS_OK) return nullptr;
    rkeys_[key] = rkey;
    return rkey;
  }

 private:
  UcxProvider* owner_;
  ucp_ep_h ep_;
  std::mutex mu_;
  std::map<uint64_t, ucp_rkey_h> rkeys_;
};

Status UcxProvider::create(UcxConfig const& cfg,
                           std::shared_ptr<UcxProvider>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  auto p = std::shared_ptr<UcxProvider>(new UcxProvider());
  Status s = p->init(cfg);
  if (s != Status::kOk) return s;
  *out = std::move(p);
  return Status::kOk;
}

Status UcxProvider::init(UcxConfig const& cfg) {
  cfg_ = cfg;

  ucp_params_t params;
  std::memset(&params, 0, sizeof(params));
  params.field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_REQUEST_SIZE |
                      UCP_PARAM_FIELD_REQUEST_INIT;
  /* RMA for the data path, AM for control messages: keeping them apart means
   * a stalled transfer cannot starve the message that would explain it. */
  params.features = UCP_FEATURE_RMA | UCP_FEATURE_AM;
  params.request_size = sizeof(UcxRequest);
  params.request_init = request_init;

  ucp_config_t* config = nullptr;
  if (ucp_config_read(nullptr, nullptr, &config) != UCS_OK)
    return Status::kDeviceError;
  ucs_status_t st = ucp_init(&params, config, &context_);
  ucp_config_release(config);
  if (st != UCS_OK) return Status::kDeviceError;

  ucp_worker_params_t wparams;
  std::memset(&wparams, 0, sizeof(wparams));
  wparams.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  /* Serialized mode, and the provider keeps that promise: every UCX call for
   * this worker is made under worker_mu_. */
  wparams.thread_mode = UCS_THREAD_MODE_SERIALIZED;
  if (ucp_worker_create(context_, &wparams, &worker_) != UCS_OK) {
    ucp_cleanup(context_);
    context_ = nullptr;
    return Status::kDeviceError;
  }
  return Status::kOk;
}

UcxProvider::~UcxProvider() {
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& kv : memories_) ucp_mem_unmap(context_, kv.second);
    memories_.clear();
  }
  if (worker_ != nullptr) ucp_worker_destroy(worker_);
  if (context_ != nullptr) ucp_cleanup(context_);
}

ProviderCaps UcxProvider::caps() const {
  ProviderCaps c;
  c.name = "ucx";
  c.supports_read = true;
  c.supports_write = true;
  c.supports_vector = false;
  c.supports_multi_qp = false;
  /* The one capability that changes how this provider has to report a write:
   * a put completing means the source is reusable, not that the data
   * arrived. poll() flushes before it says so. */
  c.needs_explicit_flush = true;
  c.supports_peer_signal = false;
  c.max_segment_bytes = 0;
  c.max_sge = 1;
  return c;
}

ProviderStats UcxProvider::stats() const {
  std::lock_guard<std::mutex> g(mu_);
  return stats_;
}

std::string UcxProvider::describe() const {
  std::ostringstream o;
  o << "{\"provider\":\"ucx\",\"needs_explicit_flush\":true"
    << ",\"advertise_ip\":\"" << cfg_.advertise_ip << "\"}";
  return o.str();
}

Status UcxProvider::register_region(void* addr, uint64_t length, DeviceId,
                                    AccessFlags, uint64_t* local_key,
                                    uint64_t* remote_key) {
  if (addr == nullptr || length == 0 || local_key == nullptr ||
      remote_key == nullptr)
    return Status::kInvalidArgument;

  ucp_mem_map_params_t mp;
  std::memset(&mp, 0, sizeof(mp));
  mp.field_mask =
      UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
  mp.address = addr;
  mp.length = static_cast<size_t>(length);

  std::lock_guard<std::mutex> w(worker_mu_);
  ucp_mem_h mem = nullptr;
  if (ucp_mem_map(context_, &mp, &mem) != UCS_OK) return Status::kDeviceError;

  /* The packed key is what a peer needs; it is opaque and must travel
   * whole. */
  void* blob = nullptr;
  size_t blob_len = 0;
  if (ucp_rkey_pack(context_, mem, &blob, &blob_len) != UCS_OK) {
    ucp_mem_unmap(context_, mem);
    return Status::kDeviceError;
  }

  std::lock_guard<std::mutex> g(mu_);
  uint64_t const key = next_key_++;
  memories_[key] = mem;
  rkey_blobs_[key].assign(static_cast<uint8_t*>(blob),
                          static_cast<uint8_t*>(blob) + blob_len);
  ucp_rkey_buffer_release(blob);
  *local_key = key;
  *remote_key = key;
  return Status::kOk;
}

Status UcxProvider::deregister_region(uint64_t local_key) {
  std::lock_guard<std::mutex> w(worker_mu_);
  ucp_mem_h mem = nullptr;
  {
    std::lock_guard<std::mutex> g(mu_);
    auto it = memories_.find(local_key);
    if (it == memories_.end()) return Status::kNotFound;
    mem = it->second;
    memories_.erase(it);
    rkey_blobs_.erase(local_key);
  }
  return ucp_mem_unmap(context_, mem) == UCS_OK ? Status::kOk
                                                : Status::kDeviceError;
}

namespace {

/* Called by UCX when an operation finishes. For a put this means the source
 * buffer is free, not that the data arrived -- the flush that follows is what
 * settles that. */
void on_rma_done(void* request, ucs_status_t status, void* /*user*/) {
  auto* u = static_cast<UcxRequest*>(request);
  u->done = true;
  u->status = status == UCS_OK ? Status::kOk : Status::kTransportError;
}

void on_flush_done(void* request, ucs_status_t status, void* /*user*/) {
  auto* u = static_cast<UcxRequest*>(request);
  u->done = true;
  u->status = status == UCS_OK ? Status::kOk : Status::kTransportError;
}

}  // namespace

Status UcxProvider::connect(std::vector<uint8_t> const& peer_metadata,
                            ProviderConnectionPtr* out) {
  if (out == nullptr || peer_metadata.empty()) return Status::kInvalidArgument;

  ucp_ep_params_t ep_params;
  std::memset(&ep_params, 0, sizeof(ep_params));
  ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
  ep_params.address =
      reinterpret_cast<ucp_address_t const*>(peer_metadata.data());

  ucp_ep_h ep = nullptr;
  {
    std::lock_guard<std::mutex> w(worker_mu_);
    if (ucp_ep_create(worker_, &ep_params, &ep) != UCS_OK)
      return Status::kPeerDisconnected;
  }
  *out = std::make_shared<UcxConnection>(this, ep);
  return Status::kOk;
}

Status UcxProvider::disconnect(ProviderConnectionPtr) { return Status::kOk; }

SubmitResult UcxProvider::submit(ProviderConnection* conn,
                                 std::vector<SubOp> const& ops) {
  SubmitResult r;
  if (conn == nullptr) {
    r.status = Status::kInvalidArgument;
    return r;
  }
  auto* c = static_cast<UcxConnection*>(conn);

  std::lock_guard<std::mutex> w(worker_mu_);
  /* Writes that completed inline. Their data may still be in flight, so they
   * are reported after one flush for the lot rather than one each. */
  std::vector<CompletionEvent> inline_writes;
  for (auto const& op : ops) {
    std::vector<uint8_t> blob;
    {
      std::lock_guard<std::mutex> g(mu_);
      auto it = rkey_blobs_.find(op.remote_key);
      if (it == rkey_blobs_.end()) {
        r.status = Status::kNotFound;
        break;
      }
      blob = it->second;
    }
    ucp_rkey_h rkey = c->rkey_for(op.remote_key, blob);
    if (rkey == nullptr) {
      r.status = Status::kTransportError;
      break;
    }

    ucp_request_param_t param;
    std::memset(&param, 0, sizeof(param));
    param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
    param.cb.send = on_rma_done;

    void* req = op.kind == SubOp::Kind::kWrite
                    ? ucp_put_nbx(c->ep(), op.local_addr,
                                  static_cast<size_t>(op.length),
                                  op.remote_addr, rkey, &param)
                    : ucp_get_nbx(c->ep(), op.local_addr,
                                  static_cast<size_t>(op.length),
                                  op.remote_addr, rkey, &param);

    CompletionEvent ev;
    ev.request = op.request;
    ev.sub_id = op.sub_id;
    ev.bytes = op.length;

    if (req == nullptr) {
      /* Completed inline. For a get that is the whole story; for a put the
       * data may still be in flight, and only a flush settles it. */
      if (op.kind == SubOp::Kind::kWrite) {
        inline_writes.push_back(ev);
      } else {
        ev.status = Status::kOk;
        std::lock_guard<std::mutex> g(mu_);
        completions_.push_back(ev);
      }
    } else if (UCS_PTR_IS_ERR(req)) {
      r.status = Status::kTransportError;
      r.provider_errno = static_cast<int32_t>(UCS_PTR_STATUS(req));
      break;
    } else {
      auto* u = static_cast<UcxRequest*>(req);
      u->request = op.request;
      u->sub_id = op.sub_id;
      u->bytes = op.length;
      u->is_write = op.kind == SubOp::Kind::kWrite;
      u->owner = this;
      u->conn = c;
      std::lock_guard<std::mutex> g(mu_);
      inflight_.push_back(req);
    }

    ++r.accepted;
    {
      std::lock_guard<std::mutex> g(mu_);
      ++stats_.subops_posted;
      stats_.payload_bytes += op.length;
      /* UCX moves the caller's memory directly; nothing is staged here. */
    }
  }

  if (!inline_writes.empty()) {
    Status const fs = flush_locked(c);
    std::lock_guard<std::mutex> g(mu_);
    for (auto& ev : inline_writes) {
      ev.status = fs;
      ev.may_have_modified_target = fs != Status::kOk;
      completions_.push_back(ev);
    }
  }
  return r;
}

Status UcxProvider::poll(uint32_t max_events,
                         std::vector<CompletionEvent>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();

  std::lock_guard<std::mutex> w(worker_mu_);
  /* Progress first: UCX makes no headway unless the worker is driven. */
  ucp_worker_progress(worker_);

  struct Finished {
    CompletionEvent ev;
    bool is_write = false;
    UcxConnection* conn = nullptr;
  };
  std::vector<Finished> finished;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = inflight_.begin(); it != inflight_.end();) {
      auto* u = static_cast<UcxRequest*>(*it);
      if (!u->done) {
        ++it;
        continue;
      }
      Finished f;
      f.ev.request = u->request;
      f.ev.sub_id = u->sub_id;
      f.ev.bytes = u->bytes;
      f.ev.status = u->status;
      f.is_write = u->is_write;
      f.conn = u->conn;
      finished.push_back(f);
      release(*it);
      it = inflight_.erase(it);
    }
  }

  /* A finished put has freed its source; the peer having the bytes takes a
   * flush. One per endpoint settles every write on it that came before, and
   * until it has, none of them is reported. */
  std::map<UcxConnection*, Status> flushed;
  for (auto const& f : finished)
    if (f.is_write && f.ev.status == Status::kOk &&
        flushed.find(f.conn) == flushed.end())
      flushed[f.conn] = flush_locked(f.conn);

  std::lock_guard<std::mutex> g(mu_);
  for (auto& f : finished) {
    if (f.is_write && f.ev.status == Status::kOk) f.ev.status = flushed[f.conn];
    f.ev.may_have_modified_target = f.ev.status != Status::kOk && f.is_write;
    completions_.push_back(f.ev);
  }

  while (!completions_.empty() && out->size() < max_events) {
    CompletionEvent const ev = completions_.front();
    if (ev.status == Status::kOk)
      ++stats_.subops_completed;
    else
      ++stats_.subops_failed;
    out->push_back(ev);
    completions_.pop_front();
  }
  return Status::kOk;
}

Status UcxProvider::flush_locked(UcxConnection* c) {
  ucp_request_param_t param;
  std::memset(&param, 0, sizeof(param));
  param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
  param.cb.send = on_flush_done;

  void* req = ucp_ep_flush_nbx(c->ep(), &param);
  if (req == nullptr) return Status::kOk;
  if (UCS_PTR_IS_ERR(req)) return Status::kTransportError;

  /* Driven to completion here. A flush is what turns "the source is free"
   * into "the peer has it", so returning before it finishes would report a
   * transfer as complete when it is not. */
  auto* u = static_cast<UcxRequest*>(req);
  while (!u->done) ucp_worker_progress(worker_);
  Status const s = u->status;
  release(req);
  return s;
}

Status UcxProvider::flush(ProviderConnection* conn) {
  if (conn == nullptr) return Status::kInvalidArgument;
  std::lock_guard<std::mutex> w(worker_mu_);
  return flush_locked(static_cast<UcxConnection*>(conn));
}

Status UcxProvider::send_control(ProviderConnection*, uint16_t type,
                                 std::vector<uint8_t> const& payload) {
  /* Control messages over UCX active messages are not wired up yet, and the
   * engine has no other channel: what it would send is refused here and
   * counted as refused there, rather than silently dropped. */
  (void)type;
  (void)payload;
  return Status::kUnsupported;
}

Status UcxProvider::poll_control(uint32_t, std::vector<ControlMessage>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  return Status::kOk;
}

Status UcxProvider::poll_peer_arrivals(uint32_t,
                                       std::vector<PeerArrival>* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->clear();
  return Status::kOk;
}

Status UcxProvider::drain(ProviderConnection* conn, int64_t) {
  return flush(conn);
}

Status UcxProvider::local_metadata(std::vector<uint8_t>* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  std::lock_guard<std::mutex> w(worker_mu_);
  ucp_address_t* addr = nullptr;
  size_t len = 0;
  if (ucp_worker_get_address(worker_, &addr, &len) != UCS_OK)
    return Status::kDeviceError;
  out->assign(reinterpret_cast<uint8_t*>(addr),
              reinterpret_cast<uint8_t*>(addr) + len);
  ucp_worker_release_address(worker_, addr);
  return Status::kOk;
}

}  // namespace hux
