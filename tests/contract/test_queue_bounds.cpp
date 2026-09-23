/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * What the engine keeps for an application that never asks for it.
 *
 * A regression. Finished requests were kept for poll_completions and ready
 * events for poll_ready_events, and neither queue had a bound. An application
 * that only waits on its requests never polls, so every request it ever made
 * stayed in memory: the 24-hour run grew by 1.2 GiB an hour on the sending
 * side, and more slowly on the receiver, which never asked for its ready
 * events either.
 *
 * A request is finished -- and its waiter woken -- before it is queued, so
 * the checks below wait for the counters to settle rather than reading them
 * the moment the last wait returns. */
#include <chrono>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

/* An engine that is its own peer: the mock delivers its control messages
 * back to it, so one engine both finishes writes and receives their ready
 * handoffs. */
struct SelfPeer {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src, dst;
  MemoryRegionPtr sreg, dreg;
  PeerPtr peer;
  RemoteRegionPtr remote;

  bool setup(EngineConfig cfg) {
    provider = std::make_shared<MockProvider>(MockConfig{});
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
    src.assign(4096, 1);
    dst.assign(4096, 0);
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
    if (dreg->export_descriptor(&desc) != Status::kOk) return false;
    return peer->import_region(desc, &remote) == Status::kOk;
  }

  /* One write, waited on rather than polled for. */
  Status write_and_wait(RequestId* id = nullptr) {
    RegionView lv, rv;
    if (sreg->view(0, 4096, &lv) != Status::kOk) return Status::kInternal;
    if (remote->view(0, 4096, &rv) != Status::kOk) return Status::kInternal;
    RequestPtr req;
    Status s = engine->write(peer.get(), lv, rv, {}, &req);
    if (s != Status::kOk) return s;
    if (id != nullptr) *id = req->id();
    return req->wait(2000);
  }
};

EngineConfig threaded() {
  EngineConfig c;
  c.progress = ProgressMode::kThread;
  return c;
}

/* Waits, briefly, for a counter to reach its final value. */
template <typename F>
bool settles(F value, uint64_t want) {
  for (int i = 0; i < 2000; ++i) {
    if (value() == want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return value() == want;
}

}  // namespace

HUX_TEST(requests_that_are_only_waited_on_are_not_kept_for_ever) {
  EngineConfig cfg = threaded();
  cfg.completion_queue_depth = 16;
  SelfPeer f;
  CHECK(f.setup(cfg));
  for (int i = 0; i < 200; ++i) CHECK_STATUS(f.write_and_wait(), Status::kOk);

  CHECK(settles([&] { return f.engine->stats().completions_dropped; }, 184));
  /* The newest sixteen are still there for a poller. */
  std::vector<RequestPtr> done;
  CHECK_STATUS(f.engine->poll_completions(1000, &done), Status::kOk);
  CHECK_EQ(done.size(), size_t(16));
}

HUX_TEST(a_depth_of_zero_keeps_no_finished_request) {
  EngineConfig cfg = threaded();
  cfg.completion_queue_depth = 0;
  SelfPeer f;
  CHECK(f.setup(cfg));
  for (int i = 0; i < 50; ++i) CHECK_STATUS(f.write_and_wait(), Status::kOk);

  CHECK(settles([&] { return f.engine->stats().completions_dropped; }, 50));
  std::vector<RequestPtr> done;
  CHECK_STATUS(f.engine->poll_completions(1000, &done), Status::kOk);
  CHECK_EQ(done.size(), size_t(0));
}

HUX_TEST(a_poller_within_the_depth_loses_nothing) {
  /* The bound is for applications that never collect. One that does, and
   * stays under the depth, must see every request exactly as before. */
  SelfPeer f;
  CHECK(f.setup(threaded()));
  for (int i = 0; i < 200; ++i) CHECK_STATUS(f.write_and_wait(), Status::kOk);

  size_t seen = 0;
  for (int i = 0; i < 2000 && seen < 200; ++i) {
    std::vector<RequestPtr> done;
    CHECK_STATUS(f.engine->poll_completions(64, &done), Status::kOk);
    seen += done.size();
    if (seen < 200) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK_EQ(seen, size_t(200));
  CHECK_EQ(f.engine->stats().completions_dropped, uint64_t(0));
}

HUX_TEST(ready_events_nobody_asks_for_are_not_kept_for_ever) {
  EngineConfig cfg = threaded();
  cfg.ready_queue_depth = 8;
  SelfPeer f;
  CHECK(f.setup(cfg));
  std::vector<RequestId> ids(50);
  for (int i = 0; i < 50; ++i)
    CHECK_STATUS(f.write_and_wait(&ids[i]), Status::kOk);

  CHECK(settles([&] { return f.engine->stats().ready_events_dropped; }, 42));
  std::vector<ReadyEventPtr> ready;
  CHECK_STATUS(f.engine->poll_ready_events(1000, &ready), Status::kOk);
  CHECK_EQ(ready.size(), size_t(8));
  /* The first eight writes' events, in order: a consumer waits on the
   * oldest first, so those are the ones kept and the later ones refused. */
  for (size_t i = 0; i < ready.size(); ++i)
    CHECK_EQ(ready[i]->request(), ids[i]);
}
