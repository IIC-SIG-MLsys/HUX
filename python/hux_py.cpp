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
#include <nanobind/stl/vector.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "transport/mock/mock_provider.h"

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

class PyEngine {
 public:
  explicit PyEngine(std::unique_ptr<Engine> e) : engine_(std::move(e)) {}

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
    AccessFlags access = AccessFlags::kLocalRead | AccessFlags::kLocalWrite;
    if (remote_read) access = access | AccessFlags::kRemoteRead;
    if (remote_write) access = access | AccessFlags::kRemoteWrite;

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

  std::shared_ptr<PyRequest> read(PyPeer& peer, PyRegion& local,
                                  PyRemoteRegion& remote, uint64_t local_offset,
                                  uint64_t remote_offset, uint64_t length) {
    return submit(peer, local, remote, local_offset, remote_offset, length,
                  false);
  }

  std::shared_ptr<PyRequest> write(PyPeer& peer, PyRegion& local,
                                   PyRemoteRegion& remote,
                                   uint64_t local_offset,
                                   uint64_t remote_offset, uint64_t length) {
    return submit(peer, local, remote, local_offset, remote_offset, length,
                  true);
  }

  /* One call, many segments: the point of a batch is that the crossing into
   * C++ happens once, not once per segment. */
  std::shared_ptr<PyRequest> readv(PyPeer& peer, PyRegion& local,
                                   PyRemoteRegion& remote,
                                   std::vector<uint64_t> const& local_offsets,
                                   std::vector<uint64_t> const& remote_offsets,
                                   std::vector<uint64_t> const& lengths) {
    return submitv(peer, local, remote, local_offsets, remote_offsets, lengths,
                   false);
  }

  std::shared_ptr<PyRequest> writev(PyPeer& peer, PyRegion& local,
                                    PyRemoteRegion& remote,
                                    std::vector<uint64_t> const& local_offsets,
                                    std::vector<uint64_t> const& remote_offsets,
                                    std::vector<uint64_t> const& lengths) {
    return submitv(peer, local, remote, local_offsets, remote_offsets, lengths,
                   true);
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

    AccessFlags access = AccessFlags::kLocalRead | AccessFlags::kLocalWrite;
    if (remote_read) access = access | AccessFlags::kRemoteRead;
    if (remote_write) access = access | AccessFlags::kRemoteWrite;

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
    d["requests_would_block"] = s.requests_would_block;
    d["submit_deferred"] = s.submit_deferred;
    d["subops_posted"] = s.subops_posted;
    d["subops_completed"] = s.subops_completed;
    d["payload_bytes"] = s.payload_bytes;
    d["payload_bytes_copied"] = s.payload_bytes_copied;
    d["registrations_created"] = s.registrations_created;
    d["registrations_reused"] = s.registrations_reused;
    d["peak_inflight_requests"] = s.peak_inflight_requests;
    return d;
  }

 private:
  std::shared_ptr<PyRequest> submit(PyPeer& peer, PyRegion& local,
                                    PyRemoteRegion& remote,
                                    uint64_t local_offset,
                                    uint64_t remote_offset, uint64_t length,
                                    bool is_write) {
    RegionView lv, rv;
    raise_on_error(local.region()->view(local_offset, length, &lv), "local view");
    raise_on_error(remote.remote()->view(remote_offset, length, &rv),
                   "remote view");
    RequestPtr r;
    Status s = is_write
                   ? engine_->write(peer.get(), lv, rv, {}, &r)
                   : engine_->read(peer.get(), lv, rv, {}, &r);
    raise_on_error(s, is_write ? "write" : "read");
    return std::make_shared<PyRequest>(std::move(r));
  }

  std::shared_ptr<PyRequest> submitv(PyPeer& peer, PyRegion& local,
                                     PyRemoteRegion& remote,
                                     std::vector<uint64_t> const& lo,
                                     std::vector<uint64_t> const& ro,
                                     std::vector<uint64_t> const& len,
                                     bool is_write) {
    if (lo.size() != ro.size() || lo.size() != len.size())
      throw std::invalid_argument(
          "local_offsets, remote_offsets and lengths must have equal length");
    std::vector<RegionView> lvs(lo.size()), rvs(ro.size());
    for (size_t i = 0; i < lo.size(); ++i) {
      raise_on_error(local.region()->view(lo[i], len[i], &lvs[i]), "local view");
      raise_on_error(remote.remote()->view(ro[i], len[i], &rvs[i]),
                     "remote view");
    }
    RequestPtr r;
    Status s = is_write ? engine_->writev(peer.get(), lvs, rvs, {}, &r)
                        : engine_->readv(peer.get(), lvs, rvs, {}, &r);
    raise_on_error(s, is_write ? "writev" : "readv");
    return std::make_shared<PyRequest>(std::move(r));
  }

  std::unique_ptr<Engine> engine_;
};

std::shared_ptr<PyEngine> make_mock_engine(bool move_data) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  MockConfig mc;
  mc.move_data = move_data;
  auto provider = std::make_shared<MockProvider>(mc);
  std::unique_ptr<Engine> e;
  raise_on_error(make_engine(cfg, nullptr, provider, &e), "make_engine");
  return std::make_shared<PyEngine>(std::move(e));
}

#ifdef HUX_PY_RDMA
std::shared_ptr<PyEngine> make_rdma_engine(std::string const& advertise_ip,
                                           uint32_t qp_per_conn,
                                           std::string const& cc_spec,
                                           uint64_t chunk_bytes) {
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
  std::unique_ptr<Engine> e;
  raise_on_error(make_engine(cfg, nullptr, provider, &e), "make_engine");
  return std::make_shared<PyEngine>(std::move(e));
}
#endif

}  // namespace

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
      .def("import_region", &PyPeer::import_region, nb::arg("descriptor"));

  nb::class_<PyRequest>(m, "Request")
      .def_prop_ro("state", &PyRequest::state)
      .def_prop_ro("done", &PyRequest::done)
      .def("reached", &PyRequest::reached, nb::arg("stage"))
      .def("wait", &PyRequest::wait, nb::arg("timeout_ms") = -1)
      .def("cancel", &PyRequest::cancel)
      .def_prop_ro("error", &PyRequest::error)
      .def_prop_ro("may_have_modified_target",
                   &PyRequest::may_have_modified_target);

  nb::class_<PyEngine>(m, "Engine")
      .def("register_memory_batch", &PyEngine::register_memory_batch,
           nb::arg("buffers"), nb::arg("remote_read") = true,
           nb::arg("remote_write") = true,
           "Register several buffers; None where one failed.")
      .def("remove_peer", &PyEngine::remove_peer, nb::arg("peer"))
      .def("progress", &PyEngine::progress,
           "One turn of the progress engine, for the explicit mode.")
      .def("notify", &PyEngine::notify, nb::arg("peer"), nb::arg("payload"),
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
      .def("add_peer", &PyEngine::add_peer, nb::arg("metadata"))
      .def("read", &PyEngine::read, nb::arg("peer"), nb::arg("local"),
           nb::arg("remote"), nb::arg("local_offset") = 0,
           nb::arg("remote_offset") = 0, nb::arg("length") = 0)
      .def("write", &PyEngine::write, nb::arg("peer"), nb::arg("local"),
           nb::arg("remote"), nb::arg("local_offset") = 0,
           nb::arg("remote_offset") = 0, nb::arg("length") = 0)
      .def("readv", &PyEngine::readv, nb::arg("peer"), nb::arg("local"),
           nb::arg("remote"), nb::arg("local_offsets"),
           nb::arg("remote_offsets"), nb::arg("lengths"))
      .def("writev", &PyEngine::writev, nb::arg("peer"), nb::arg("local"),
           nb::arg("remote"), nb::arg("local_offsets"),
           nb::arg("remote_offsets"), nb::arg("lengths"))
      .def("poll", &PyEngine::poll, nb::arg("max_items") = 32)
      .def("close", &PyEngine::close, nb::arg("timeout_ms") = 5000)
      .def("describe", &PyEngine::describe)
      .def("deregister_memory", &PyEngine::deregister_memory,
           nb::arg("region"),
           "Retire a region; the registration goes when nothing holds it.")
      .def("release_cached_registrations",
           &PyEngine::release_cached_registrations,
           "Release registrations the reuse cache holds; returns how many.")
      .def("stats", &PyEngine::stats);

  m.def("make_mock_engine", &make_mock_engine, nb::arg("move_data") = true,
        "An engine over the mock backend, for testing without hardware.");
#ifdef HUX_PY_RDMA
  m.def("make_rdma_engine", &make_rdma_engine, nb::arg("advertise_ip"),
        nb::arg("qp_per_conn") = 1, nb::arg("cc") = "off",
        nb::arg("chunk_bytes") = 0);
#endif
}
