/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Scheduling fairness between requests competing for the same budget.
 *
 * A transport that admits requests in arrival order is fine until one of them
 * is large. Then a small request behind it waits for the whole thing, and the
 * delay is invisible in aggregate throughput -- which is exactly why it needs
 * a test rather than a benchmark. */
#include <cstring>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

/* Records the order sub-operations reached the provider, which is what
 * fairness is actually about -- request completion order follows from it. */
struct OrderingFixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> buf;
  MemoryRegionPtr local, remote_src_region;
  PeerPtr peer;
  RemoteRegionPtr remote;

  bool setup(uint64_t chunk_bytes, uint64_t budget) {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    cfg.chunk_bytes = chunk_bytes;
    MockConfig mc;
    mc.move_data = false;
    /* A budget small enough that both requests cannot be in flight at once,
     * which is what makes the order they are offered in observable. */
    mc.budget_bytes = budget;
    mc.submit_status_on_partial = Status::kWouldBlock;
    provider = std::make_shared<MockProvider>(mc);
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;

    buf.assign(1 << 20, 0);
    if (engine->register_memory(buf.data(), buf.size(),
                                AccessFlags::kRemoteRead,
                                &remote_src_region) != Status::kOk)
      return false;
    if (engine->register_memory(buf.data(), buf.size(),
                                AccessFlags::kLocalWrite,
                                &local) != Status::kOk)
      return false;
    std::vector<uint8_t> meta, desc;
    engine->local_metadata(&meta);
    if (engine->add_peer(meta, &peer) != Status::kOk) return false;
    remote_src_region->export_descriptor(&desc);
    return peer->import_region(desc, &remote) == Status::kOk;
  }

  Status submit(uint64_t bytes, RequestPtr* out) {
    RegionView lv, rv;
    if (local->view(0, bytes, &lv) != Status::kOk) return Status::kOutOfRange;
    if (remote->view(0, bytes, &rv) != Status::kOk) return Status::kOutOfRange;
    return engine->read(peer.get(), lv, rv, {}, out);
  }
};

}  // namespace

HUX_TEST(a_small_request_does_not_wait_for_a_large_one) {
  OrderingFixture f;
  /* 64 KiB chunks, and room for two of them at a time. The large request is
   * 16 chunks, the small one is 1. */
  CHECK(f.setup(64 << 10, 128 << 10));

  RequestPtr big, small;
  CHECK_STATUS(f.submit(16 << 16, &big), Status::kOk);   /* 16 chunks */
  CHECK_STATUS(f.submit(64 << 10, &small), Status::kOk); /* 1 chunk */

  /* Drive until the small request finishes, counting passes. Under arrival
   * order it would have to wait out the large one entirely. */
  int passes = 0;
  std::vector<RequestPtr> done;
  for (; passes < 400; ++passes) {
    f.engine->poll_completions(32, &done);
    bool fin = false;
    small->test(&fin);
    if (fin) break;
  }

  CHECK(small->state() == RequestState::kSucceeded);
  /* The large request needs at least 8 passes on its own. If the small one
   * only finished after that, it was queued behind it rather than interleaved.
   */
  std::printf("       small finished after %d passes\n", passes);
  CHECK(passes < 8);
}

HUX_TEST(both_requests_still_complete_in_full) {
  /* Interleaving must not cost completeness: fairness that drops work is not
   * fairness. */
  OrderingFixture f;
  CHECK(f.setup(64 << 10, 128 << 10));

  RequestPtr a, b;
  CHECK_STATUS(f.submit(8 << 16, &a), Status::kOk);
  CHECK_STATUS(f.submit(4 << 16, &b), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0;
       i < 600 && !(is_terminal(a->state()) && is_terminal(b->state())); ++i)
    f.engine->poll_completions(32, &done);

  CHECK(a->state() == RequestState::kSucceeded);
  CHECK(b->state() == RequestState::kSucceeded);

  EngineStats st = f.engine->stats();
  /* 8 + 4 chunks, every one of them posted and completed exactly once. */
  CHECK_EQ(st.subops_posted, 12u);
  CHECK_EQ(st.subops_completed, 12u);
}

/* The rotation test above still passes with an unlimited quantum, because the
 * provider's own budget already limits what one turn can place. The quantum
 * does something different and needs its own case: even a provider willing to
 * take everything must not let a single request occupy one scheduling turn
 * entirely. */
HUX_TEST(a_turn_submits_no_more_than_the_quantum) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  cfg.chunk_bytes = 64 << 10;
  /* Room for four chunks per turn. */
  cfg.scheduler_quantum_bytes = 256 << 10;

  MockConfig mc;
  mc.move_data = false; /* no budget: the provider accepts whatever it gets */
  auto provider = std::make_shared<MockProvider>(mc);
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &engine), Status::kOk);

  std::vector<uint8_t> buf(1 << 20, 0);
  MemoryRegionPtr src, dst;
  CHECK_STATUS(engine->register_memory(buf.data(), buf.size(),
                                       AccessFlags::kRemoteRead, &src),
               Status::kOk);
  CHECK_STATUS(engine->register_memory(buf.data(), buf.size(),
                                       AccessFlags::kLocalWrite, &dst),
               Status::kOk);
  std::vector<uint8_t> meta, desc;
  engine->local_metadata(&meta);
  PeerPtr peer;
  CHECK_STATUS(engine->add_peer(meta, &peer), Status::kOk);
  src->export_descriptor(&desc);
  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kOk);

  RegionView lv, rv;
  CHECK_STATUS(dst->view(0, 1 << 20, &lv), Status::kOk); /* 16 chunks */
  CHECK_STATUS(remote->view(0, 1 << 20, &rv), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(engine->read(peer.get(), lv, rv, {}, &req), Status::kOk);

  /* Submission happens on the calling thread for the first turn, so exactly
   * one quantum should have gone out: four chunks, not sixteen. */
  EngineStats after_submit = engine->stats();
  std::printf("       first turn posted %llu of 16 chunks\n",
              (unsigned long long)after_submit.subops_posted);
  CHECK_EQ(after_submit.subops_posted, 4u);

  /* The rest follows on later turns, and all of it arrives. */
  std::vector<RequestPtr> done;
  for (int i = 0; i < 200 && !is_terminal(req->state()); ++i)
    engine->poll_completions(32, &done);
  CHECK(req->state() == RequestState::kSucceeded);
  CHECK_EQ(engine->stats().subops_posted, 16u);
}
