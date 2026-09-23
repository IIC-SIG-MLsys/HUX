/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * A request that cannot succeed has to stop posting before it says so.
 *
 * Regressions. A request split across two adapters is posted lane by lane.
 * When the first lane was refused outright the request was failed on the
 * spot -- FailedSafe published, its entry dropped -- and the second lane was
 * then posted anyway: the NIC read a source its caller had been told was
 * free, the completions that followed were thrown away for belonging to
 * nothing, and posting moved the failed request back to in-flight, so it no
 * longer read as done. Separately, a cancelled request that was waiting on a
 * dependency stayed queued until the dependency was met, and then ran. */
#include <cstring>
#include <vector>

#include "core/engine_impl.h"
#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

constexpr uint64_t kChunk = 64 << 10;

/* One engine reached over two sibling transports, so a transfer of two
 * chunks goes out one chunk per lane, the first lane first. */
struct TwoLanes {
  std::shared_ptr<MockProvider> first, second;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src, dst;
  MemoryRegionPtr sreg, dreg;
  PeerPtr peer;
  RemoteRegionPtr remote;

  bool make(MockConfig a, MockConfig b) {
    a.name = "mock";
    b.name = "mock#1";
    first = std::make_shared<MockProvider>(a);
    second = std::make_shared<MockProvider>(b);
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    cfg.chunk_bytes = kChunk;
    std::vector<TransportProviderPtr> provs{first, second};
    if (make_engine(cfg, nullptr, provs, &engine) != Status::kOk) return false;
    src.assign(2 * kChunk, 0x5a);
    dst.assign(2 * kChunk, 0);
    if (engine->register_memory(src.data(), src.size(), AccessFlags::kLocalRead,
                                &sreg) != Status::kOk)
      return false;
    if (engine->register_memory(
            dst.data(), dst.size(),
            AccessFlags::kLocalWrite | AccessFlags::kRemoteWrite,
            &dreg) != Status::kOk)
      return false;
    std::vector<uint8_t> meta, desc;
    if (engine->local_metadata(&meta) != Status::kOk) return false;
    if (engine->add_peer(meta, &peer) != Status::kOk) return false;
    if (peer->caps().lane_count != 2u) return false;
    if (dreg->export_descriptor(&desc) != Status::kOk) return false;
    return peer->import_region(desc, &remote) == Status::kOk;
  }

  Status write_both_lanes(RequestPtr* out) {
    RegionView lv, rv;
    if (sreg->view(0, 2 * kChunk, &lv) != Status::kOk) return Status::kInternal;
    if (remote->view(0, 2 * kChunk, &rv) != Status::kOk)
      return Status::kInternal;
    return engine->write(peer.get(), lv, rv, {}, out);
  }

  void pump(int n) {
    std::vector<RequestPtr> done;
    for (int i = 0; i < n; ++i) engine->poll_completions(32, &done);
  }
};

class PendingEvent : public DeviceEvent {
 public:
  DeviceId device() const override { return DeviceId{}; }
  bool recorded() const override { return true; }
  void* native_handle() const override { return nullptr; }
  Status query(bool* complete) override {
    *complete = false; /* never finishes */
    return Status::kOk;
  }
};

}  // namespace

HUX_TEST(a_lane_refused_outright_stops_the_others_from_posting) {
  MockConfig refuse;
  refuse.reject_all = true;
  TwoLanes t;
  CHECK(t.make(refuse, MockConfig{}));

  RequestPtr req;
  t.write_both_lanes(&req);
  CHECK(req != nullptr);
  t.pump(50);

  /* The second lane never reached its transport, so nothing of the request
   * was ever read by a NIC and FailedSafe is true. */
  CHECK_EQ(t.second->submitted_subops(), 0u);
  CHECK(req->state() == RequestState::kFailed);
  CHECK(req->reached(Stage::kFailedSafe));
  CHECK(!req->error().may_have_modified_target);
  CHECK_EQ(t.engine->stats().requests_failed, uint64_t(1));
}

HUX_TEST(a_lane_refused_after_another_went_out_waits_for_it) {
  /* The other order: the first lane is on the wire and never completes, the
   * second is refused. The request must not be failed while the first lane
   * is still reading the source. */
  MockConfig hold;
  hold.never_complete = true;
  MockConfig refuse;
  refuse.reject_all = true;
  TwoLanes t;
  CHECK(t.make(hold, refuse));

  RequestPtr req;
  t.write_both_lanes(&req);
  CHECK(req != nullptr);
  t.pump(50);

  CHECK_EQ(t.first->submitted_subops(), 1u);
  CHECK(!is_terminal(req->state()));
  CHECK_STATUS(req->wait(20), Status::kTimeout);
}

HUX_TEST(a_request_keeps_every_lane_it_went_out_on) {
  /* A regression. A request held the first lane's connection and only the
   * peer held the others, so removing and dropping the peer destroyed the
   * second lane's queue pairs with the request's work still on them: none
   * of it could complete, and the request never ended. */
  MockConfig hold;
  hold.never_complete = true;
  TwoLanes t;
  CHECK(t.make(hold, hold));
  RequestPtr req;
  CHECK_STATUS(t.write_both_lanes(&req), Status::kOk);
  CHECK_EQ(t.second->submitted_subops(), 1u);

  std::weak_ptr<ProviderConnection> second =
      static_cast<PeerImpl*>(t.peer.get())->lanes()[1].conn;
  CHECK_STATUS(t.engine->remove_peer(t.peer), Status::kOk);
  t.peer.reset();

  /* The work is still on it, so the connection must be too. */
  CHECK(!second.expired());
  CHECK(!is_terminal(req->state()));
}

HUX_TEST(a_transport_whose_poll_fails_still_hands_back_what_it_took) {
  /* A regression. progress() returned at the first transport whose poll
   * failed: what that poll had handed back was dropped, and the transports
   * after it were not polled at all, so a request with work on either never
   * ended. The failure is still reported. */
  MockConfig failing;
  failing.poll_status = Status::kTransportError;
  TwoLanes t;
  CHECK(t.make(failing, MockConfig{}));
  RequestPtr req;
  CHECK_STATUS(t.write_both_lanes(&req), Status::kOk);

  std::vector<RequestPtr> done;
  Status last = Status::kOk;
  for (int i = 0; i < 50 && done.empty(); ++i)
    last = t.engine->poll_completions(32, &done);
  CHECK_EQ(done.size(), size_t(1));
  CHECK(req->state() == RequestState::kSucceeded);
  CHECK_STATUS(last, Status::kTransportError);
}

HUX_TEST(cancelling_a_request_waiting_on_a_dependency_ends_it) {
  auto provider = std::make_shared<MockProvider>(MockConfig{});
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &engine), Status::kOk);
  std::vector<uint8_t> src(4096, 1), dst(4096, 0);
  MemoryRegionPtr sreg, dreg;
  CHECK_STATUS(engine->register_memory(src.data(), src.size(),
                                       AccessFlags::kRemoteRead, &sreg),
               Status::kOk);
  CHECK_STATUS(engine->register_memory(dst.data(), dst.size(),
                                       AccessFlags::kLocalWrite, &dreg),
               Status::kOk);
  std::vector<uint8_t> meta, desc;
  CHECK_STATUS(engine->local_metadata(&meta), Status::kOk);
  PeerPtr peer;
  CHECK_STATUS(engine->add_peer(meta, &peer), Status::kOk);
  CHECK_STATUS(sreg->export_descriptor(&desc), Status::kOk);
  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kOk);
  RegionView lv, rv;
  CHECK_STATUS(dreg->view(0, 4096, &lv), Status::kOk);
  CHECK_STATUS(remote->view(0, 4096, &rv), Status::kOk);

  TransferOptions opts;
  opts.after.push_back(std::make_shared<PendingEvent>());
  RequestPtr req;
  CHECK_STATUS(engine->read(peer.get(), lv, rv, opts, &req), Status::kOk);
  CHECK_STATUS(req->cancel(), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 50 && !is_terminal(req->state()); ++i)
    engine->poll_completions(16, &done);

  /* Ended without ever running: the producer it was waiting for is not a
   * reason to keep a cancelled request. */
  CHECK(req->state() == RequestState::kCancelled);
  CHECK(req->reached(Stage::kCancelledSafe));
  CHECK_EQ(provider->submitted_subops(), 0u);
  CHECK_EQ(engine->stats().requests_cancelled, uint64_t(1));
}
