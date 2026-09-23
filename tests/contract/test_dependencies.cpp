/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Device dependencies, with a fake event so the states that matter can be
 * produced exactly: recorded and complete, recorded and pending, and never
 * recorded at all. The last one is the trap -- an unrecorded event captures
 * no work, so treating it as satisfied would release the NIC against data
 * that does not exist yet. */
#include <cstring>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

class FakeEvent : public DeviceEvent {
 public:
  FakeEvent(bool recorded, bool complete)
      : recorded_(recorded), complete_(complete) {}

  DeviceId device() const override { return DeviceId{}; }
  bool recorded() const override { return recorded_; }
  void* native_handle() const override { return nullptr; }
  Status query(bool* complete) override {
    if (!recorded_) {
      *complete = false;
      return Status::kInvalidArgument;
    }
    *complete = complete_;
    return Status::kOk;
  }
  void complete() { complete_ = true; }

 private:
  bool recorded_;
  bool complete_;
};

struct Fixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src, dst;
  MemoryRegionPtr src_region, dst_region;
  PeerPtr peer;
  RemoteRegionPtr remote_src;

  bool setup(size_t bytes = 4096) {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    provider = std::make_shared<MockProvider>(MockConfig{});
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
    src.assign(bytes, 0xAB);
    dst.assign(bytes, 0);
    if (engine->register_memory(src.data(), bytes, AccessFlags::kRemoteRead,
                                &src_region) != Status::kOk)
      return false;
    if (engine->register_memory(dst.data(), bytes, AccessFlags::kLocalWrite,
                                &dst_region) != Status::kOk)
      return false;
    std::vector<uint8_t> meta;
    engine->local_metadata(&meta);
    if (engine->add_peer(meta, &peer) != Status::kOk) return false;
    std::vector<uint8_t> desc;
    src_region->export_descriptor(&desc);
    return peer->import_region(desc, &remote_src) == Status::kOk;
  }

  void views(RegionView* l, RegionView* r, uint64_t n = 4096) {
    dst_region->view(0, n, l);
    remote_src->view(0, n, r);
  }
};

}  // namespace

HUX_TEST(pending_dependency_holds_submission_not_the_caller) {
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  auto ev = std::make_shared<FakeEvent>(true, false); /* recorded, pending */
  TransferOptions opts;
  opts.after.push_back(ev);

  RequestPtr req;
  /* Returns immediately: the request is accepted, only its submission waits. */
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &req), Status::kOk);
  CHECK(req->state() == RequestState::kWaitDependency);

  /* Nothing may reach the provider while the dependency is outstanding. */
  std::vector<RequestPtr> done;
  f.engine->poll_completions(16, &done);
  CHECK_EQ(f.provider->submitted_subops(), 0u);
  CHECK(req->state() == RequestState::kWaitDependency);

  ev->complete();
  for (int i = 0; i < 100 && !is_terminal(req->state()); ++i)
    f.engine->poll_completions(16, &done);

  CHECK(req->state() == RequestState::kSucceeded);
  CHECK(f.provider->submitted_subops() > 0u);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

/* Reproduces the HIP behaviour the roadmap calls out: a query on an event
 * that was never recorded reports success. Taking that answer at face value
 * would release the NIC against data that does not exist yet, so recorded
 * state has to be checked in its own right, not inferred from the query. */
class HipLikeEvent : public DeviceEvent {
 public:
  DeviceId device() const override { return DeviceId{}; }
  bool recorded() const override { return false; }
  void* native_handle() const override { return nullptr; }
  Status query(bool* complete) override {
    *complete = true; /* success for work that was never captured */
    return Status::kOk;
  }
};

HUX_TEST(unrecorded_event_never_satisfies_a_dependency) {
  /* And is refused outright. It used to be accepted and waited on, which
   * released nothing against it -- and never ended: an event captures no
   * work until recorded and is not a promise of work to come. */
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  auto ev = std::make_shared<HipLikeEvent>();
  TransferOptions opts;
  opts.after.push_back(ev);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &req),
               Status::kInvalidArgument);
  CHECK(req == nullptr);
  CHECK_EQ(f.provider->submitted_subops(), 0u);
  CHECK_EQ(f.engine->stats().requests_accepted, uint64_t(0));
}

/* Recorded, pending, and then its query starts failing -- a sticky device
 * error does exactly this. */
class BreakingEvent : public DeviceEvent {
 public:
  DeviceId device() const override { return DeviceId{}; }
  bool recorded() const override { return true; }
  void* native_handle() const override { return nullptr; }
  Status query(bool* complete) override {
    *complete = false;
    return broken_ ? Status::kDeviceError : Status::kOk;
  }
  void break_it() { broken_ = true; }

 private:
  bool broken_ = false;
};

HUX_TEST(a_dependency_that_can_no_longer_complete_fails_the_request) {
  /* A regression: a query that failed read the same as one still pending,
   * so the request waited for ever. Nothing of it was posted, so it can end
   * failed and safe. */
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);
  auto ev = std::make_shared<BreakingEvent>();
  TransferOptions opts;
  opts.after.push_back(ev);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &req), Status::kOk);
  CHECK(req->state() == RequestState::kWaitDependency);

  ev->break_it();
  std::vector<RequestPtr> done;
  for (int i = 0; i < 50 && !is_terminal(req->state()); ++i)
    f.engine->poll_completions(16, &done);

  CHECK(req->state() == RequestState::kFailed);
  CHECK(req->reached(Stage::kFailedSafe));
  CHECK_STATUS(req->error().status, Status::kDeviceError);
  CHECK_EQ(f.provider->submitted_subops(), 0u);

  /* And one already failing when submitted is refused on the way in. */
  RequestPtr again;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &again),
               Status::kDeviceError);
  CHECK(again == nullptr);
}

HUX_TEST(met_dependency_submits_straight_away) {
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  auto ev = std::make_shared<FakeEvent>(true, true);
  TransferOptions opts;
  opts.after.push_back(ev);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &req), Status::kOk);
  /* Already satisfied, so it goes straight out rather than queueing. */
  CHECK(req->state() != RequestState::kWaitDependency);
  CHECK(f.provider->submitted_subops() > 0u);
}

HUX_TEST(all_dependencies_must_be_met_not_just_one) {
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  auto done_ev = std::make_shared<FakeEvent>(true, true);
  auto pending_ev = std::make_shared<FakeEvent>(true, false);
  TransferOptions opts;
  opts.after.push_back(done_ev);
  opts.after.push_back(pending_ev);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &req), Status::kOk);
  std::vector<RequestPtr> done;
  f.engine->poll_completions(16, &done);
  CHECK(req->state() == RequestState::kWaitDependency);
  CHECK_EQ(f.provider->submitted_subops(), 0u);

  pending_ev->complete();
  for (int i = 0; i < 100 && !is_terminal(req->state()); ++i)
    f.engine->poll_completions(16, &done);
  CHECK(req->state() == RequestState::kSucceeded);
}

HUX_TEST(waiting_request_can_still_be_cancelled) {
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  auto ev = std::make_shared<FakeEvent>(true, false);
  TransferOptions opts;
  opts.after.push_back(ev);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &req), Status::kOk);
  CHECK(req->state() == RequestState::kWaitDependency);

  /* Nothing was posted, so cancelling costs nothing and must not leave the
   * request stuck waiting on an event that may never fire. */
  CHECK_STATUS(req->cancel(), Status::kOk);
  CHECK(req->state() == RequestState::kDraining);
  CHECK_EQ(f.provider->submitted_subops(), 0u);
}

HUX_TEST(a_request_that_ended_cancelled_waits_as_cancelled) {
  /* wait() answers straight away for a request that has already ended, and
   * that answer did not look for a cancellation: a transfer cancelled before
   * it was waited on reported success, with nothing moved. */
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  auto ev = std::make_shared<FakeEvent>(true, false);
  TransferOptions opts;
  opts.after.push_back(ev);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &req), Status::kOk);
  CHECK_STATUS(req->cancel(), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 20 && !is_terminal(req->state()); ++i)
    f.engine->poll_completions(16, &done);
  CHECK(req->state() == RequestState::kCancelled);

  CHECK_STATUS(req->wait(0), Status::kCancelled);
  CHECK_STATUS(req->wait(-1), Status::kCancelled);
}

/* ---- Copy accounting ---- */

/* A zero in payload_bytes_copied only means something if the counter can also
 * be non-zero. The mock genuinely memcpys, so it must report that; a path that
 * transfers in place reports nothing. Without this pair, "zero-copy" would be
 * indistinguishable from a counter nobody increments. */
HUX_TEST(a_copying_provider_reports_what_it_copied) {
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, {}, &req), Status::kOk);
  std::vector<RequestPtr> done;
  for (int i = 0; i < 200 && !is_terminal(req->state()); ++i)
    f.engine->poll_completions(16, &done);

  EngineStats st = f.engine->stats();
  CHECK_EQ(st.payload_bytes, 4096u);
  CHECK_EQ(st.payload_bytes_copied, 4096u); /* the mock really copied */
  CHECK_EQ(st.requests_accepted, 1u);
  CHECK_EQ(st.requests_succeeded, 1u);
  CHECK(st.subops_posted > 0u);
}

HUX_TEST(stats_count_failures_and_would_block_separately) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  cfg.max_inflight_requests = 1;
  MockConfig mock;
  mock.move_data = false;
  auto provider = std::make_shared<MockProvider>(mock);
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &engine), Status::kOk);

  std::vector<uint8_t> src(4096, 1), dst(4096, 0);
  MemoryRegionPtr sr, dr;
  CHECK_STATUS(
      engine->register_memory(src.data(), 4096, AccessFlags::kRemoteRead, &sr),
      Status::kOk);
  CHECK_STATUS(
      engine->register_memory(dst.data(), 4096, AccessFlags::kLocalWrite, &dr),
      Status::kOk);
  std::vector<uint8_t> meta, desc;
  engine->local_metadata(&meta);
  PeerPtr peer;
  CHECK_STATUS(engine->add_peer(meta, &peer), Status::kOk);
  sr->export_descriptor(&desc);
  RemoteRegionPtr rr;
  CHECK_STATUS(peer->import_region(desc, &rr), Status::kOk);

  RegionView lv, rv;
  dr->view(0, 512, &lv);
  rr->view(0, 512, &rv);

  RequestPtr a, b;
  CHECK_STATUS(engine->read(peer.get(), lv, rv, {}, &a), Status::kOk);
  /* Second request exceeds max_inflight_requests: not accepted, no network
   * side effect, and counted apart from a failure. */
  CHECK_STATUS(engine->read(peer.get(), lv, rv, {}, &b), Status::kWouldBlock);

  EngineStats st = engine->stats();
  CHECK_EQ(st.requests_accepted, 1u);
  CHECK_EQ(st.requests_would_block, 1u);
  CHECK_EQ(st.requests_failed, 0u);
  /* move_data is off, so nothing was copied even though the mock is not a
   * zero-copy transport. */
  CHECK_EQ(st.payload_bytes_copied, 0u);
}
