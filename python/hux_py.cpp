/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Python bindings.
 *
 * Three things decide whether these are safe to use, and none of them is
 * visible from the Python side:
 *
 *   - A registration holds a reference to the object whose memory it covers.
 *     Without it the object can be collected while the NIC still has its
 *     buffer registered, and what follows is a use-after-free with no Python
 *     traceback to explain it.
 *   - Every call that can block releases the GIL. One that does not freezes
 *     every other thread in the interpreter, including whichever one would
 *     have driven progress.
 *   - Completions are collected by polling, never by calling into Python from
 *     a progress thread. A callback from a thread that does not hold the GIL
 *     is a crash; acquiring it there would let the transport stall on
 *     interpreter contention. */
#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "hux/host_memory.h"
#include "hux/device.h"
#include "transport/mock/mock_provider.h"

#if defined(HUX_PY_CUDA)
#include "device/cuda_backend.h"
#elif defined(HUX_PY_ROCM)
#include "device/rocm_backend.h"
#elif defined(HUX_PY_NEUWARE)
#include "device/neuware_backend.h"
#elif defined(HUX_PY_MUSA)
#include "device/musa_backend.h"
#endif

#ifdef HUX_PY_RDMA
#include "transport/cc/controller.h"
#include "transport/rdma/rdma_provider.h"
#endif

namespace nb = nanobind;
using namespace hux;

namespace {

void raise_on_error(Status s, char const* what) {
  if (s == Status::kOk) return;
  throw std::runtime_error(std::string(what) + ": " + to_string(s));
}

/* A registered region, holding the buffer it covers.
 *
 * The Py_buffer is what keeps the memory alive: PyObject_GetBuffer takes a
 * reference to the exporting object and holds it until PyBuffer_Release. A
 * caller that registers a tensor and drops every reference to it has every
 * reason to expect the transfer to keep working, and this is what makes that
 * true. No separate reference is kept, because a second one would suggest the
 * buffer alone is not enough. */
class PyRegion {
 public:
  PyRegion(MemoryRegionPtr region, Py_buffer view)
      : region_(std::move(region)), view_(view) {}
  /* An object Python promises is immutable -- bytes, a read-only memoryview.
   * Registered for reading only, and never the destination of a read. */
  bool read_only() const { return view_.readonly != 0; }

  ~PyRegion() {
    /* Destruction can happen after the interpreter has finalized, when object
     * lifetimes no longer follow program order. Releasing the buffer then
     * would crash with nothing in the stack to attribute it to, and the
     * process is going away regardless. */
    if (!Py_IsInitialized()) return;
    nb::gil_scoped_acquire gil;
    PyBuffer_Release(&view_);
  }

  MemoryRegionPtr const& region() const { return region_; }
  /* Gives up this object's hold on the registration. The engine drops its own
   * when told to deregister, and the registration itself goes when the last
   * reference does -- so a caller that only dropped the Python object would
   * still be holding one here. */
  void release_handle() { region_.reset(); }
  uint64_t length() const { return region_ != nullptr ? region_->length() : 0; }
  uint64_t id() const { return region_ != nullptr ? region_->id() : 0; }

  nb::bytes descriptor() const {
    if (region_ == nullptr)
      throw std::runtime_error("region has been deregistered");
    std::vector<uint8_t> d;
    raise_on_error(region_->export_descriptor(&d), "export_descriptor");
    return nb::bytes(reinterpret_cast<char const*>(d.data()), d.size());
  }

 private:
  MemoryRegionPtr region_;
  Py_buffer view_;  /* holds a reference to the exporting object */
};

class PyRemoteRegion {
 public:
  explicit PyRemoteRegion(RemoteRegionPtr r) : remote_(std::move(r)) {}
  RemoteRegionPtr const& remote() const { return remote_; }
  uint64_t length() const { return remote_->length(); }
  bool valid() const { return remote_->valid(); }

 private:
  RemoteRegionPtr remote_;
};

class PyPeer {
 public:
  explicit PyPeer(PeerPtr p) : peer_(std::move(p)) {}
  Peer* get() const { return peer_.get(); }
  PeerPtr shared() const { return peer_; }
  uint64_t id() const { return peer_->id(); }
  uint32_t epoch() const { return peer_->epoch(); }
  bool connected() const { return peer_->connected(); }

  /* Which transport this peer is actually reached over, and where it turned
   * out to be. Without this a fallback to a slower path shows up only as
   * unexplained slowness, which is the thing this library is supposed to make
   * impossible. */
  nb::dict caps() const {
    PeerCaps c = peer_->caps();
    nb::dict d;
    d["path"] = to_string(c.path);
    d["place"] = to_string(c.place);
    d["provider"] = c.provider;
    d["qp_count"] = c.qp_count;
    d["remote_max_registration_bytes"] = c.remote_max_registration_bytes;
    return d;
  }

  std::shared_ptr<PyRemoteRegion> import_region(nb::bytes const& desc) {
    std::vector<uint8_t> d(desc.c_str(), desc.c_str() + desc.size());
    RemoteRegionPtr r;
    raise_on_error(peer_->import_region(d, &r), "import_region");
    return std::make_shared<PyRemoteRegion>(std::move(r));
  }

 private:
  PeerPtr peer_;
};

class PyRequest {
 public:
  explicit PyRequest(RequestPtr r) : req_(std::move(r)) {}

  std::string state() const { return to_string(req_->state()); }
  bool done() const {
    bool d = false;
    req_->test(&d);
    return d;
  }
  bool reached(std::string const& stage) const {
    if (stage == "accepted") return req_->reached(Stage::kAccepted);
    if (stage == "source_reusable") return req_->reached(Stage::kSourceReusable);
    if (stage == "transfer_complete")
      return req_->reached(Stage::kTransferComplete);
    if (stage == "target_ready") return req_->reached(Stage::kTargetReady);
    if (stage == "failed_safe") return req_->reached(Stage::kFailedSafe);
    if (stage == "cancelled_safe") return req_->reached(Stage::kCancelledSafe);
    throw std::invalid_argument("unknown stage: " + stage);
  }

  /* Blocking, so the GIL goes. A wait that held it would stop every other
   * thread, including one polling for the completion being waited on. */
  std::string wait(int64_t timeout_ms) {
    Status s;
    {
      nb::gil_scoped_release release;
      s = req_->wait(timeout_ms);
    }
    return to_string(s);
  }

  std::string cancel() { return to_string(req_->cancel()); }

  std::optional<std::string> error() const {
    if (req_->error().ok()) return std::nullopt;
    return std::string(to_string(req_->error().status));
  }
  bool may_have_modified_target() const {
    return req_->error().may_have_modified_target;
  }

 private:
  RequestPtr req_;
};

/* An execution queue the application already owns. Adapted, not created:
 * a transfer has to be ordered against the stream the caller's kernels run
 * on, and a stream this library made would not be that one.
 *
 * Constructed from the integer a framework exposes -- torch's
 * `torch.cuda.Stream.cuda_stream`, for instance -- because there is no
 * portable stream object to pass, and every framework hands out the native
 * handle as an address. */
class PyStream {
 public:
  explicit PyStream(DeviceStreamPtr s) : stream_(std::move(s)) {}
  DeviceStream* get() const { return stream_.get(); }

 private:
  DeviceStreamPtr stream_;
};

/* A point in a stream. Passing one to a transfer says the adapter may not
 * touch the buffers until the work before it has finished, which is what
 * makes a transfer safe to issue while a kernel is still writing its
 * source. */
class PyEvent {
 public:
  explicit PyEvent(DeviceEventPtr e) : event_(std::move(e)) {}
  DeviceEvent* get() const { return event_.get(); }
  DeviceEventPtr shared() const { return event_; }

 private:
  DeviceEventPtr event_;
};

class PyEngine {
 public:
  explicit PyEngine(std::unique_ptr<Engine> e,
                    std::shared_ptr<DeviceBackend> dev = nullptr)
      : engine_(std::move(e)), device_(std::move(dev)) {}

#ifdef HUX_PY_RDMA
  /* Takes one connection a peer dialled, and keeps it.
   *
   * A peer that only serves -- its memory read by others, dialling nobody
   * itself -- has no add_peer to make, and without this nothing on its side
   * completes the handshake: the dialler's add_peer fails with
   * kPeerDisconnected. The connection is held here because the transfers
   * that arrive over it are one-sided, so nothing else refers to it. */
  bool accept(int64_t timeout_ms) {
    if (rdma_ == nullptr)
      throw std::runtime_error("accept is only for an RDMA engine");
    ProviderConnectionPtr conn;
    Status s;
    {
      nb::gil_scoped_release release;
      s = rdma_->accept(timeout_ms, &conn);
    }
    if (s == Status::kTimeout) return false;
    raise_on_error(s, "accept");
    accepted_.push_back(std::move(conn));
    return true;
  }
  void set_rdma(std::shared_ptr<RdmaProvider> p) { rdma_ = std::move(p); }
#endif

  /* Takes anything supporting the buffer protocol -- numpy arrays, torch
   * tensors, bytearrays. The buffer must be contiguous: a strided view has
   * gaps the NIC would happily transfer over. */
  std::shared_ptr<PyRegion> register_memory(nb::object obj,
                                            bool remote_read = true,
                                            bool remote_write = true) {
    Py_buffer view;
    if (PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE | PyBUF_WRITABLE) != 0) {
      PyErr_Clear();
      if (PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE) != 0)
        throw std::invalid_argument("object does not support the buffer protocol");
    }
    AccessFlags const access = access_for(view, remote_read, remote_write);

    MemoryRegionPtr region;
    Status s = engine_->register_memory(view.buf,
                                        static_cast<uint64_t>(view.len),
                                        access, &region);
    if (s != Status::kOk) {
      PyBuffer_Release(&view);
      raise_on_error(s, "register_memory");
    }
    return std::make_shared<PyRegion>(std::move(region), view);
  }

  nb::bytes local_metadata() const {
    std::vector<uint8_t> m;
    raise_on_error(engine_->local_metadata(&m), "local_metadata");
    return nb::bytes(reinterpret_cast<char const*>(m.data()), m.size());
  }

  std::shared_ptr<PyPeer> add_peer(nb::bytes const& meta) {
    std::vector<uint8_t> m(meta.c_str(), meta.c_str() + meta.size());
    PeerPtr p;
    {
      /* Connecting talks to another host; holding the GIL through it would
       * freeze the interpreter for the duration. */
      nb::gil_scoped_release release;
      raise_on_error(engine_->add_peer(m, &p), "add_peer");
    }
    return std::make_shared<PyPeer>(std::move(p));
  }

  std::shared_ptr<PyRequest> read(
      PyPeer& peer, PyRegion& local, PyRemoteRegion& remote,
      uint64_t local_offset, uint64_t remote_offset, uint64_t length,
      std::vector<std::shared_ptr<PyEvent>> const& after) {
    return submit(peer, local, remote, local_offset, remote_offset, length,
                  false, after);
  }

  std::shared_ptr<PyRequest> write(
      PyPeer& peer, PyRegion& local, PyRemoteRegion& remote,
      uint64_t local_offset, uint64_t remote_offset, uint64_t length,
      std::vector<std::shared_ptr<PyEvent>> const& after, bool ready_handoff) {
    return submit(peer, local, remote, local_offset, remote_offset, length,
                  true, after, ready_handoff);
  }

  /* One call, many segments: the point of a batch is that the crossing into
   * C++ happens once, not once per segment. */
  std::shared_ptr<PyRequest> readv(
      PyPeer& peer, PyRegion& local, PyRemoteRegion& remote,
      std::vector<uint64_t> const& local_offsets,
      std::vector<uint64_t> const& remote_offsets,
      std::vector<uint64_t> const& lengths,
      std::vector<std::shared_ptr<PyEvent>> const& after) {
    return submitv(peer, local, remote, local_offsets, remote_offsets, lengths,
                   false, after);
  }

  std::shared_ptr<PyRequest> writev(
      PyPeer& peer, PyRegion& local, PyRemoteRegion& remote,
      std::vector<uint64_t> const& local_offsets,
      std::vector<uint64_t> const& remote_offsets,
      std::vector<uint64_t> const& lengths,
      std::vector<std::shared_ptr<PyEvent>> const& after, bool ready_handoff) {
    return submitv(peer, local, remote, local_offsets, remote_offsets, lengths,
                   true, after, ready_handoff);
  }

  /* Returns how many requests finished. Completions are collected here rather
   * than delivered by a callback: calling into Python from a progress thread
   * would need the GIL, and taking it there lets interpreter contention stall
   * the transport. */
  size_t poll(uint32_t max_items) {
    std::vector<RequestPtr> done;
    {
      nb::gil_scoped_release release;
      engine_->poll_completions(max_items, &done);
    }
    return done.size();
  }

  std::string close(int64_t timeout_ms) {
    Status s;
    {
      nb::gil_scoped_release release;
      s = engine_->close(timeout_ms);
    }
    return to_string(s);
  }

  std::string describe() const { return engine_->describe(); }

  /* Adapt the caller's stream. The argument is the native handle as an
   * integer, which is how every framework exposes it. */
  std::shared_ptr<PyStream> import_stream(uintptr_t native) {
    if (device_ == nullptr)
      throw std::runtime_error("no device backend in this build");
    DeviceStreamPtr s;
    raise_on_error(
        device_->import_stream(reinterpret_cast<void*>(native), &s),
        "import_stream");
    return std::make_shared<PyStream>(std::move(s));
  }

  /* A point in that stream, to hand to a transfer as something it must wait
   * for. */
  std::shared_ptr<PyEvent> record_event(PyStream& stream) {
    if (device_ == nullptr)
      throw std::runtime_error("no device backend in this build");
    DeviceEventPtr e;
    raise_on_error(engine_->record_event(stream.get(), &e), "record_event");
    return std::make_shared<PyEvent>(std::move(e));
  }

  /* The other direction: the caller's later kernels wait for something the
   * transfer produced, without synchronising the whole device. */
  void stream_wait_event(PyStream& stream, PyEvent& event) {
    if (device_ == nullptr)
      throw std::runtime_error("no device backend in this build");
    raise_on_error(device_->stream_wait_event(stream.get(), event.get()),
                   "stream_wait_event");
  }

  bool has_device() const { return device_ != nullptr; }

  void remove_peer(std::shared_ptr<PyPeer> peer) {
    if (peer == nullptr) return;
    PeerPtr p = peer->shared();
    nb::gil_scoped_release release;
    engine_->remove_peer(std::move(p));
  }

  /* One turn of the progress engine, for the explicit mode. Releasing the
   * GIL matters here: this is where submission, completion and control
   * traffic actually happen, and holding it would stop every other Python
   * thread for the duration. */
  void progress() {
    nb::gil_scoped_release release;
    engine_->progress();
  }

  /* An application message to a peer, independent of any transfer. The
   * request it returns completes when the peer acknowledges it, not when the
   * bytes leave -- which is the only version of "delivered" worth waiting
   * on. */
  std::shared_ptr<PyRequest> notify(PyPeer& peer, nb::bytes const& payload) {
    std::vector<uint8_t> p(payload.c_str(), payload.c_str() + payload.size());
    RequestPtr req;
    {
      nb::gil_scoped_release release;
      raise_on_error(engine_->notify(peer.get(), p, &req), "notify");
    }
    return std::make_shared<PyRequest>(std::move(req));
  }

  nb::list poll_notifications(uint32_t max_items) {
    std::vector<Notification> notes;
    {
      nb::gil_scoped_release release;
      engine_->poll_notifications(max_items, &notes);
    }
    nb::list out;
    for (auto const& n : notes) {
      nb::dict d;
      d["peer"] = n.peer;
      d["id"] = n.id;
      d["related_request"] = n.related_request;
      d["payload"] = nb::bytes(
          reinterpret_cast<char const*>(n.payload.data()), n.payload.size());
      out.append(d);
    }
    return out;
  }

  /* What a peer wrote here, and exactly which bytes. A target owner needs
   * the span before it can schedule anything that consumes the data. */
  nb::list poll_ready_events(uint32_t max_items) {
    std::vector<ReadyEventPtr> events;
    {
      nb::gil_scoped_release release;
      engine_->poll_ready_events(max_items, &events);
    }
    nb::list out;
    for (auto const& e : events) {
      if (e == nullptr) continue;
      nb::dict d;
      d["request"] = e->request();
      d["peer"] = e->peer();
      d["region"] = e->region();
      d["generation"] = e->generation();
      d["offset"] = e->span().offset;
      d["length"] = e->span().length;
      out.append(d);
    }
    return out;
  }

  /* The same registration, for many buffers in one call. A failure on one
   * item does not lose the others: the list has a Region where it worked and
   * None where it did not, in the order given. */
  nb::list register_memory_batch(nb::sequence objs, bool remote_read = true,
                                 bool remote_write = true) {
    std::vector<Py_buffer> views;
    std::vector<void*> addrs;
    std::vector<uint64_t> lengths;
    for (auto item : objs) {
      Py_buffer v;
      nb::object o = nb::borrow(item);
      if (PyObject_GetBuffer(o.ptr(), &v, PyBUF_SIMPLE | PyBUF_WRITABLE) != 0) {
        PyErr_Clear();
        if (PyObject_GetBuffer(o.ptr(), &v, PyBUF_SIMPLE) != 0) {
          for (auto& held : views) PyBuffer_Release(&held);
          throw std::invalid_argument(
              "an object does not support the buffer protocol");
        }
      }
      views.push_back(v);
      addrs.push_back(v.buf);
      lengths.push_back(static_cast<uint64_t>(v.len));
    }

    /* One access for the whole batch, so one read-only buffer in it makes
     * all of them read-only rather than letting the adapter write into it. */
    bool any_read_only = false;
    for (auto const& v : views) any_read_only = any_read_only || v.readonly;
    AccessFlags access = AccessFlags::kLocalRead;
    if (!any_read_only) access = access | AccessFlags::kLocalWrite;
    if (remote_read) access = access | AccessFlags::kRemoteRead;
    if (remote_write && !any_read_only)
      access = access | AccessFlags::kRemoteWrite;

    std::vector<RegistrationResult> results;
    Status s = engine_->register_memory_batch(addrs, lengths, access, &results);
    if (s != Status::kOk || results.size() != views.size()) {
      for (auto& held : views) PyBuffer_Release(&held);
      raise_on_error(s == Status::kOk ? Status::kInternal : s,
                     "register_memory_batch");
    }

    nb::list out;
    for (size_t i = 0; i < results.size(); ++i) {
      if (results[i].status != Status::kOk || results[i].region == nullptr) {
        /* Released here: nothing will hold this buffer, and leaving it
         * exported would pin the caller's object for the life of the
         * process. */
        PyBuffer_Release(&views[i]);
        out.append(nb::none());
        continue;
      }
      out.append(
          nb::cast(std::make_shared<PyRegion>(results[i].region, views[i])));
    }
    return out;
  }

  /* Stops new transfers against this region and lets the engine go of it.
   * The registration itself survives while anything still references it --
   * another handle, or the reuse cache -- which is what
   * release_cached_registrations is for. */
  void deregister_memory(PyRegion& region) {
    if (region.region() == nullptr) return;
    MemoryRegionPtr r = region.region();
    region.release_handle();
    nb::gil_scoped_release release;
    engine_->deregister_memory(std::move(r));
  }

  /* Releases what the reuse cache is holding on nobody's behalf. Deregistering
   * a handle does not do this -- the cache keeps the registration for the next
   * caller -- so a caller about to free or unmap the memory needs it. It can
   * block, which is why the GIL is dropped: the IPC path waits here for the
   * peer to confirm it unmapped. */
  uint32_t release_cached_registrations() {
    uint32_t n = 0;
    {
      nb::gil_scoped_release release;
      engine_->release_cached_registrations(&n);
    }
    return n;
  }

  nb::dict stats() const {
    EngineStats s = engine_->stats();
    nb::dict d;
    d["requests_accepted"] = s.requests_accepted;
    d["requests_succeeded"] = s.requests_succeeded;
    d["requests_failed"] = s.requests_failed;
    d["requests_cancelled"] = s.requests_cancelled;
    d["requests_would_block"] = s.requests_would_block;
    d["submit_deferred"] = s.submit_deferred;
    d["requests_waiting_on_dependency"] = s.requests_waiting_on_dependency;
    d["subops_posted"] = s.subops_posted;
    d["subops_completed"] = s.subops_completed;
    d["subops_failed"] = s.subops_failed;
    d["payload_bytes"] = s.payload_bytes;
    d["payload_bytes_copied"] = s.payload_bytes_copied;
    d["registrations_created"] = s.registrations_created;
    d["registrations_reused"] = s.registrations_reused;
    d["registration_cache_size"] = s.registration_cache_size;
    d["peak_inflight_requests"] = s.peak_inflight_requests;
    /* What the engine let go because nobody collected it. A Python loop
     * that counts what poll() returns against what it submitted needs these
     * to explain a shortfall rather than wait on it for ever. */
    d["completions_dropped"] = s.completions_dropped;
    d["ready_events_dropped"] = s.ready_events_dropped;
    /* The control-plane counters, which nothing here exposed. Two of them
     * are the only place a failure shows at all: a ready handoff or an
     * acknowledgement the transport refused leaves the peer waiting while
     * the local request succeeds. */
    d["notifications_sent"] = s.notifications_sent;
    d["notifications_received"] = s.notifications_received;
    d["notifications_dropped"] = s.notifications_dropped;
    d["notification_acks_failed"] = s.notification_acks_failed;
    d["ready_handoffs_sent"] = s.ready_handoffs_sent;
    d["ready_handoffs_failed"] = s.ready_handoffs_failed;
    d["ready_handoffs_received"] = s.ready_handoffs_received;
    d["region_invalidates_sent"] = s.region_invalidates_sent;
    d["region_invalidates_failed"] = s.region_invalidates_failed;
    return d;
  }

 private:
  /* A read-only buffer is registered for reading and nothing else. The
   * adapter honours whatever it is given, so granting a write -- by a peer,
   * or local, as the destination of a read -- would let it write into an
   * object Python promises is immutable, and a small bytes object may be
   * shared across the whole interpreter. */
  static AccessFlags access_for(Py_buffer const& view, bool remote_read,
                                bool remote_write) {
    bool const read_only = view.readonly != 0;
    AccessFlags access = AccessFlags::kLocalRead;
    if (!read_only) access = access | AccessFlags::kLocalWrite;
    if (remote_read) access = access | AccessFlags::kRemoteRead;
    if (remote_write && !read_only) access = access | AccessFlags::kRemoteWrite;
    return access;
  }

  /* Checked here because the region below is null once deregistered, and
   * using it would be a crash rather than an exception. */
  static void usable_for(PyRegion const& local, bool is_write) {
    if (local.region() == nullptr)
      throw std::runtime_error("region has been deregistered");
    if (!is_write && local.read_only())
      throw std::invalid_argument(
          "the destination of a read must be writable; this buffer is "
          "read-only");
  }

  static TransferOptions options_from(
      std::vector<std::shared_ptr<PyEvent>> const& after,
      bool ready_handoff = true) {
    TransferOptions o;
    o.ready_handoff = ready_handoff;
    for (auto const& e : after)
      if (e != nullptr) o.after.push_back(e->shared());
    return o;
  }

  std::shared_ptr<PyRequest> submit(
      PyPeer& peer, PyRegion& local, PyRemoteRegion& remote,
      uint64_t local_offset, uint64_t remote_offset, uint64_t length,
      bool is_write, std::vector<std::shared_ptr<PyEvent>> const& after,
      bool ready_handoff = true) {
    usable_for(local, is_write);
    /* The default: the rest of the local region. A length of zero used to
     * go through as a transfer of nothing, which never ended; the engine
     * now refuses one, and a default that could only fail is no default. */
    if (length == 0) {
      uint64_t const n = local.region()->length();
      length = local_offset < n ? n - local_offset : 0;
    }
    RegionView lv, rv;
    raise_on_error(local.region()->view(local_offset, length, &lv), "local view");
    raise_on_error(remote.remote()->view(remote_offset, length, &rv),
                   "remote view");
    TransferOptions const opts = options_from(after, ready_handoff);
    RequestPtr r;
    Status s;
    {
      /* Nothing the interpreter owns is touched here, and holding the GIL
       * across a submission stops every other Python thread. poll() releases
       * it for the same reason. */
      nb::gil_scoped_release release;
      s = is_write ? engine_->write(peer.get(), lv, rv, opts, &r)
                   : engine_->read(peer.get(), lv, rv, opts, &r);
    }
    raise_on_error(s, is_write ? "write" : "read");
    return std::make_shared<PyRequest>(std::move(r));
  }

  std::shared_ptr<PyRequest> submitv(PyPeer& peer, PyRegion& local,
                                     PyRemoteRegion& remote,
                                     std::vector<uint64_t> const& lo,
                                     std::vector<uint64_t> const& ro,
                                     std::vector<uint64_t> const& len,
                                     bool is_write,
                                     std::vector<std::shared_ptr<PyEvent>> const&
                                         after,
                                     bool ready_handoff = true) {
    if (lo.size() != ro.size() || lo.size() != len.size())
      throw std::invalid_argument(
          "local_offsets, remote_offsets and lengths must have equal length");
    usable_for(local, is_write);
    std::vector<RegionView> lvs(lo.size()), rvs(ro.size());
    for (size_t i = 0; i < lo.size(); ++i) {
      raise_on_error(local.region()->view(lo[i], len[i], &lvs[i]), "local view");
      raise_on_error(remote.remote()->view(ro[i], len[i], &rvs[i]),
                     "remote view");
    }
    TransferOptions const opts = options_from(after, ready_handoff);
    RequestPtr r;
    Status s;
    {
      /* See submit(); a vector submission is the longest of these calls. */
      nb::gil_scoped_release release;
      s = is_write ? engine_->writev(peer.get(), lvs, rvs, opts, &r)
                   : engine_->readv(peer.get(), lvs, rvs, opts, &r);
    }
    raise_on_error(s, is_write ? "writev" : "readv");
    return std::make_shared<PyRequest>(std::move(r));
  }

  std::unique_ptr<Engine> engine_;
  /* Outlives the engine's use of it: the engine holds a raw pointer. */
  std::shared_ptr<DeviceBackend> device_;
#ifdef HUX_PY_RDMA
  std::shared_ptr<RdmaProvider> rdma_;
  std::vector<ProviderConnectionPtr> accepted_;
#endif
};

/* Whichever backend was compiled in. Only one can be, because the vendor
 * runtimes are not co-installable in one process, so this is a choice made
 * at build time rather than at run time. */
Status make_device_backend(int index, std::shared_ptr<DeviceBackend>* out) {
#if defined(HUX_PY_CUDA)
  return CudaBackend::create(index, out);
#elif defined(HUX_PY_ROCM)
  return RocmBackend::create(index, out);
#elif defined(HUX_PY_NEUWARE)
  return NeuwareBackend::create(index, out);
#elif defined(HUX_PY_MUSA)
  return MusaBackend::create(index, out);
#else
  (void)index;
  (void)out;
  return Status::kUnsupported;
#endif
}

bool device_backend_available() {
#if defined(HUX_PY_CUDA) || defined(HUX_PY_ROCM) || defined(HUX_PY_NEUWARE) || \
    defined(HUX_PY_MUSA)
  return true;
#else
  return false;
#endif
}

std::shared_ptr<PyEngine> make_mock_engine(bool move_data, int gpu) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  MockConfig mc;
  mc.move_data = move_data;
  auto provider = std::make_shared<MockProvider>(mc);
  std::shared_ptr<DeviceBackend> dev;
  if (gpu >= 0) raise_on_error(make_device_backend(gpu, &dev), "device");
  std::unique_ptr<Engine> e;
  raise_on_error(make_engine(cfg, dev, provider, &e), "make_engine");
  return std::make_shared<PyEngine>(std::move(e), std::move(dev));
}

#ifdef HUX_PY_RDMA
std::shared_ptr<PyEngine> make_rdma_engine(std::string const& advertise_ip,
                                           uint32_t qp_per_conn,
                                           std::string const& cc_spec,
                                           uint64_t chunk_bytes, int gpu) {
  RdmaConfig rc;
  rc.advertise_ip = advertise_ip;
  rc.qp_per_conn = qp_per_conn;
  if (cc_spec == "timely") rc.cc = make_cc_timely();
  else if (cc_spec.rfind("fixed:", 0) == 0)
    rc.cc = make_cc_fixed_window(std::strtoull(cc_spec.c_str() + 6, nullptr, 10));
  else rc.cc = make_cc_off();

  std::shared_ptr<RdmaProvider> provider;
  {
    nb::gil_scoped_release release;
    raise_on_error(RdmaProvider::create(rc, &provider), "RdmaProvider::create");
  }
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  if (chunk_bytes > 0) cfg.chunk_bytes = chunk_bytes;
  std::shared_ptr<DeviceBackend> dev;
  if (gpu >= 0) raise_on_error(make_device_backend(gpu, &dev), "device");
  std::unique_ptr<Engine> e;
  raise_on_error(make_engine(cfg, dev, provider, &e), "make_engine");
  auto py = std::make_shared<PyEngine>(std::move(e), std::move(dev));
  py->set_rdma(std::move(provider));
  return py;
}
#endif

}  // namespace

/* Host memory on 2 MiB pages, exposed through the buffer protocol so it
 * registers like any other buffer -- and wraps into numpy or torch without a
 * copy. A view taken of it holds a reference, so the memory outlives every
 * view and every registration made from one. */
struct PyHostBuffer {
  HostAllocation a;
  PyHostBuffer() = default;
  PyHostBuffer(PyHostBuffer const&) = delete;
  PyHostBuffer& operator=(PyHostBuffer const&) = delete;
  ~PyHostBuffer() { free_host(a); }
};

int host_buffer_getbuffer(PyObject* self, Py_buffer* view, int flags) {
  PyHostBuffer* b = nb::inst_ptr<PyHostBuffer>(self);
  return PyBuffer_FillInfo(view, self, b->a.ptr,
                           static_cast<Py_ssize_t>(b->a.bytes), 0, flags);
}

PyType_Slot host_buffer_slots[] = {
    {Py_bf_getbuffer, reinterpret_cast<void*>(host_buffer_getbuffer)},
    {0, nullptr}};

std::unique_ptr<PyHostBuffer> alloc_host_py(uint64_t nbytes) {
  auto b = std::make_unique<PyHostBuffer>();
  raise_on_error(alloc_host(nbytes, &b->a), "alloc_host");
  return b;
}

NB_MODULE(hux, m) {
  m.doc() = "HUX: heterogeneous unified exchange";

  nb::class_<PyRegion>(m, "Region")
      .def_prop_ro("length", &PyRegion::length)
      .def_prop_ro("id", &PyRegion::id)
      .def("descriptor", &PyRegion::descriptor,
           "Serialized descriptor to hand to a peer.");

  nb::class_<PyRemoteRegion>(m, "RemoteRegion")
      .def_prop_ro("length", &PyRemoteRegion::length)
      .def_prop_ro("valid", &PyRemoteRegion::valid);

  nb::class_<PyPeer>(m, "Peer")
      .def_prop_ro("id", &PyPeer::id)
      .def_prop_ro("epoch", &PyPeer::epoch)
      .def_prop_ro("connected", &PyPeer::connected)
      .def("caps", &PyPeer::caps,
           "The transport in use, where the peer is, and the provider's name.")
      /* A remote region is looked up through the peer it was imported
       * into, so the peer has to outlive it. */
      .def("import_region", &PyPeer::import_region, nb::arg("descriptor"),
           nb::keep_alive<0, 1>());

  nb::class_<PyRequest>(m, "Request")
      .def_prop_ro("state", &PyRequest::state)
      .def_prop_ro("done", &PyRequest::done)
      .def("reached", &PyRequest::reached, nb::arg("stage"))
      .def("wait", &PyRequest::wait, nb::arg("timeout_ms") = -1)
      .def("cancel", &PyRequest::cancel)
      .def_prop_ro("error", &PyRequest::error)
      .def_prop_ro("may_have_modified_target",
                   &PyRequest::may_have_modified_target);

  /* Opaque on purpose: both are handles to something the vendor runtime
   * owns, and there is nothing useful for Python to read out of them. */
  nb::class_<PyStream>(m, "Stream");
  nb::class_<PyEvent>(m, "Event");

  nb::class_<PyEngine>(m, "Engine")
      .def("register_memory_batch", &PyEngine::register_memory_batch,
           nb::arg("buffers"), nb::arg("remote_read") = true,
           nb::arg("remote_write") = true,
           "Register several buffers; None where one failed.")
      .def("remove_peer", &PyEngine::remove_peer, nb::arg("peer"))
      .def("progress", &PyEngine::progress,
           "One turn of the progress engine, for the explicit mode.")
      .def("notify", &PyEngine::notify, nb::arg("peer"), nb::arg("payload"),
           nb::keep_alive<0, 1>(),
           "Send an application message; completes on the peer's"
           " acknowledgement.")
      .def("poll_notifications", &PyEngine::poll_notifications,
           nb::arg("max_items") = 32)
      .def("poll_ready_events", &PyEngine::poll_ready_events,
           nb::arg("max_items") = 32)
      .def("register_memory", &PyEngine::register_memory, nb::arg("buffer"),
           nb::arg("remote_read") = true, nb::arg("remote_write") = true,
           "Registers an object supporting the buffer protocol. The engine "
           "keeps a reference to it for as long as the registration lives.")
      .def("local_metadata", &PyEngine::local_metadata)
      /* A peer holds a plain pointer to the engine -- importing a region
       * writes into the engine's table -- so dropping the engine while
       * Python still has the peer would leave it pointing at freed memory.
       * Every object below is tied to the engine for the same reason: a
       * request needs the engine's progress to ever complete, and a stream
       * or event belongs to the device backend the engine holds. Regions
       * are deliberately not tied: their registration keeps the providers
       * alive on its own, so one outliving the engine is allowed. */
      .def("add_peer", &PyEngine::add_peer, nb::arg("metadata"),
           nb::keep_alive<0, 1>())
      .def("read", &PyEngine::read, nb::arg("peer"), nb::arg("local"),
           nb::arg("remote"), nb::arg("local_offset") = 0,
           nb::arg("remote_offset") = 0, nb::arg("length") = 0,
           nb::arg("after") = std::vector<std::shared_ptr<PyEvent>>{},
           nb::keep_alive<0, 1>(),
           "Read into local from remote. length=0 means the rest of the"
           " local region from local_offset.")
      .def("write", &PyEngine::write, nb::arg("peer"), nb::arg("local"),
           nb::arg("remote"), nb::arg("local_offset") = 0,
           nb::arg("remote_offset") = 0, nb::arg("length") = 0,
           nb::arg("after") = std::vector<std::shared_ptr<PyEvent>>{},
           nb::arg("ready_handoff") = true, nb::keep_alive<0, 1>(),
           "Write local to remote. length=0 means the rest of the local"
           " region from local_offset. ready_handoff=False tells the peer"
           " nothing when it lands, for a caller that signals arrival"
           " itself; it saves a socket write per transfer.")
      .def("readv", &PyEngine::readv, nb::arg("peer"), nb::arg("local"),
           nb::arg("remote"), nb::arg("local_offsets"),
           nb::arg("remote_offsets"), nb::arg("lengths"),
           nb::arg("after") = std::vector<std::shared_ptr<PyEvent>>{},
           nb::keep_alive<0, 1>())
      .def("writev", &PyEngine::writev, nb::arg("peer"), nb::arg("local"),
           nb::arg("remote"), nb::arg("local_offsets"),
           nb::arg("remote_offsets"), nb::arg("lengths"),
           nb::arg("after") = std::vector<std::shared_ptr<PyEvent>>{},
           nb::arg("ready_handoff") = true, nb::keep_alive<0, 1>())
      .def("import_stream", &PyEngine::import_stream, nb::arg("native_handle"),
           nb::keep_alive<0, 1>(),
           "Adapt an execution queue the application already owns, given as"
           " the native handle -- torch.cuda.Stream.cuda_stream, for"
           " instance.")
      .def("record_event", &PyEngine::record_event, nb::arg("stream"),
           nb::keep_alive<0, 1>(),
           "A point in that stream, to pass to a transfer as `after`.")
      .def("stream_wait_event", &PyEngine::stream_wait_event,
           nb::arg("stream"), nb::arg("event"),
           "Make later work on the stream wait for the event, rather than"
           " synchronising the whole device.")
      .def("has_device", &PyEngine::has_device,
           "Whether this engine was built with a device backend; the stream"
           " and event calls need one.")
      .def("poll", &PyEngine::poll, nb::arg("max_items") = 32)
      .def("close", &PyEngine::close, nb::arg("timeout_ms") = 5000)
      .def("describe", &PyEngine::describe)
      .def("deregister_memory", &PyEngine::deregister_memory,
           nb::arg("region"),
           "Retire a region; the registration goes when nothing holds it.")
      .def("release_cached_registrations",
           &PyEngine::release_cached_registrations,
           "Release registrations the reuse cache holds; returns how many.")
      .def("stats", &PyEngine::stats)
#ifdef HUX_PY_RDMA
      .def("accept", &PyEngine::accept, nb::arg("timeout_ms") = 0,
           "Take one connection a peer dialled, and keep it. True if one"
           " arrived, False on timeout. An engine whose memory others read,"
           " and which dials nobody itself, has to call this or their"
           " add_peer fails -- nothing else completes the handshake.")
#endif
      ;

  nb::class_<PyHostBuffer>(m, "HostBuffer", nb::type_slots(host_buffer_slots))
      .def_prop_ro("nbytes", [](PyHostBuffer const& b) { return b.a.bytes; })
      .def_prop_ro("huge_bytes",
                   [](PyHostBuffer const& b) { return b.a.huge_bytes; },
                   "How much of it the kernel put on huge pages.")
      .def("__len__", [](PyHostBuffer const& b) { return b.a.bytes; });
  m.def("alloc_host", &alloc_host_py, nb::arg("nbytes"),
        "Host memory for buffers the adapter moves, on 2 MiB pages where the"
        " kernel grants them, rounded up to whole pages and zero-filled."
        " Where an IOMMU translates the adapter's accesses, ordinary 4 KiB"
        " pages can cost most of a large transfer's bandwidth.");
  m.def("make_mock_engine", &make_mock_engine, nb::arg("move_data") = true,
        nb::arg("gpu") = -1,
        "An engine over the mock backend, for testing without hardware.");
#ifdef HUX_PY_RDMA
  m.def("make_rdma_engine", &make_rdma_engine, nb::arg("advertise_ip"),
        nb::arg("qp_per_conn") = 1, nb::arg("cc") = "off",
        nb::arg("chunk_bytes") = 0, nb::arg("gpu") = -1);
#endif
  m.def("device_backend_available", &device_backend_available,
        "Whether this build has a device backend at all. Which vendor is"
        " fixed at build time, because their runtimes do not co-install.");
}
