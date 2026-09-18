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
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk) return false;
    src.assign(bytes, 0xAB);
    dst.assign(bytes, 0);
    if (engine->register_memory(src.data(), bytes, AccessFlags::kRemoteRead,
                                &src_region) != Status::kOk) return false;
    if (engine->register_memory(dst.data(), bytes, AccessFlags::kLocalWrite,
                                &dst_region) != Status::kOk) return false;
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

  auto ev = std::make_shared<FakeEvent>(true, false);  /* recorded, pending */
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
    *complete = true;      /* success for work that was never captured */
    return Status::kOk;
  }
};

HUX_TEST(unrecorded_event_never_satisfies_a_dependency) {
  Fixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  auto ev = std::make_shared<HipLikeEvent>();
  TransferOptions opts;
  opts.after.push_back(ev);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, opts, &req), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 200; ++i) f.engine->poll_completions(16, &done);

  CHECK(req->state() == RequestState::kWaitDependency);
  CHECK_EQ(f.provider->submitted_subops(), 0u);
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
