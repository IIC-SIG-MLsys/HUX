/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Combinations the single-feature tests cannot reach: several threads
 * submitting at once, requests of mixed sizes, cancellations landing while
 * other work is in flight.
 *
 * What is checked is invariants rather than outcomes. Under concurrency the
 * outcome of any one request is not predictable -- it may succeed, be
 * cancelled, or be refused for want of budget -- but the books still have to
 * balance: everything accepted ends somewhere, everything posted completes
 * once, and nothing is left holding resources. */
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

struct StressFixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> pool;
  MemoryRegionPtr src, dst;
  PeerPtr peer;
  RemoteRegionPtr remote;

  bool setup(EngineConfig cfg, MockConfig mc) {
    provider = std::make_shared<MockProvider>(mc);
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
    pool.assign(4 << 20, 0);
    if (engine->register_memory(pool.data(), 2 << 20, AccessFlags::kRemoteRead,
                                &src) != Status::kOk)
      return false;
    if (engine->register_memory(pool.data() + (2 << 20), 2 << 20,
                                AccessFlags::kLocalWrite, &dst) != Status::kOk)
      return false;
    std::vector<uint8_t> meta, desc;
    engine->local_metadata(&meta);
    if (engine->add_peer(meta, &peer) != Status::kOk) return false;
    src->export_descriptor(&desc);
    return peer->import_region(desc, &remote) == Status::kOk;
  }
};

}  // namespace

HUX_TEST(concurrent_mixed_sizes_keep_the_books_balanced) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread; /* a background thread drives it */
  cfg.chunk_bytes = 64 << 10;
  cfg.max_inflight_requests = 64;
  MockConfig mc;
  mc.move_data = false;
  mc.budget_bytes = 4 << 20; /* forces deferral under load */

  StressFixture f;
  CHECK(f.setup(cfg, mc));

  constexpr int kThreads = 6;
  constexpr int kPerThread = 40;
  std::atomic<int> accepted{0};
  std::atomic<int> refused{0};
  std::vector<RequestPtr> all;
  std::mutex all_mu;

  auto worker = [&](int seed) {
    std::mt19937 rng(static_cast<uint32_t>(seed));
    /* Sizes spanning two orders of magnitude, so short requests queue behind
     * long ones and the scheduler has something to do. */
    std::uniform_int_distribution<int> pick(0, 3);
    uint64_t const sizes[] = {4096, 64 << 10, 256 << 10, 1 << 20};

    for (int i = 0; i < kPerThread; ++i) {
      uint64_t const bytes = sizes[pick(rng)];
      RegionView lv, rv;
      if (f.dst->view(0, bytes, &lv) != Status::kOk) continue;
      if (f.remote->view(0, bytes, &rv) != Status::kOk) continue;

      RequestPtr r;
      Status s = f.engine->read(f.peer.get(), lv, rv, {}, &r);
      if (s == Status::kOk) {
        accepted.fetch_add(1);
        std::lock_guard<std::mutex> g(all_mu);
        all.push_back(std::move(r));
      } else {
        /* Refusal is a legitimate outcome under load, and must not be
         * confused with failure: nothing was sent. */
        refused.fetch_add(1);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t + 1);
  for (auto& t : threads) t.join();

  /* Everything accepted must reach a terminal state. One that never does
   * holds its regions and its connection for good. */
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  size_t terminal = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    terminal = 0;
    {
      std::lock_guard<std::mutex> g(all_mu);
      for (auto const& r : all)
        if (is_terminal(r->state())) ++terminal;
      if (terminal == all.size()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::printf("       %d accepted, %d refused, %zu of %zu terminal\n",
              accepted.load(), refused.load(), terminal, all.size());
  CHECK_EQ(terminal, all.size());

  EngineStats const st = f.engine->stats();
  CHECK_EQ(st.requests_accepted,
           st.requests_succeeded + st.requests_failed + st.requests_cancelled);
  CHECK_EQ(st.subops_posted, st.subops_completed + st.subops_failed);
  /* Refusals were counted apart from failures. */
  CHECK_EQ(st.requests_failed, 0u);
  CHECK(st.requests_would_block + st.submit_deferred > 0u);
}

HUX_TEST(cancellations_racing_with_completions_still_settle) {
  /* Cancelling work that may already have finished is the case a caller hits
   * on shutdown. Either answer is correct; hanging is not. */
  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread;
  cfg.chunk_bytes = 16 << 10;
  MockConfig mc;
  mc.move_data = false;

  StressFixture f;
  CHECK(f.setup(cfg, mc));

  std::vector<RequestPtr> reqs;
  for (int i = 0; i < 100; ++i) {
    RegionView lv, rv;
    if (f.dst->view(0, 128 << 10, &lv) != Status::kOk) continue;
    if (f.remote->view(0, 128 << 10, &rv) != Status::kOk) continue;
    RequestPtr r;
    if (f.engine->read(f.peer.get(), lv, rv, {}, &r) == Status::kOk)
      reqs.push_back(std::move(r));
  }

  /* Cancel from another thread while the engine is still working. */
  std::thread canceller([&] {
    for (size_t i = 0; i < reqs.size(); i += 3) reqs[i]->cancel();
  });
  canceller.join();

  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  size_t settled = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    settled = 0;
    for (auto const& r : reqs)
      if (is_terminal(r->state())) ++settled;
    if (settled == reqs.size()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::printf("       %zu of %zu settled after cancellation\n", settled,
              reqs.size());
  CHECK_EQ(settled, reqs.size());

  /* Whatever the outcome, a cancelled request must never claim its target is
   * ready. */
  for (auto const& r : reqs) {
    if (r->state() == RequestState::kCancelled)
      CHECK(!r->reached(Stage::kTargetReady));
  }
}

HUX_TEST(many_registrations_do_not_accumulate) {
  /* A caller that registers and releases in a loop must not grow the process
   * without bound, whatever the cache holds. */
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  cfg.registration_cache_entries = 8;
  auto provider = std::make_shared<MockProvider>(MockConfig{});
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &engine), Status::kOk);

  std::vector<uint8_t> pool(8 << 20, 0);
  for (int round = 0; round < 200; ++round) {
    MemoryRegionPtr r;
    size_t const offset = static_cast<size_t>(round % 64) * (64 << 10);
    CHECK_STATUS(engine->register_memory(pool.data() + offset, 4096,
                                         AccessFlags::kRemoteRead, &r),
                 Status::kOk);
    CHECK_STATUS(engine->deregister_memory(r), Status::kOk);
  }

  EngineStats const st = engine->stats();
  std::printf("       %llu created, %llu reused, %llu cached\n",
              (unsigned long long)st.registrations_created,
              (unsigned long long)st.registrations_reused,
              (unsigned long long)st.registration_cache_size);
  CHECK(st.registration_cache_size <= cfg.registration_cache_entries);
  CHECK(provider->live_registrations() <= cfg.registration_cache_entries);
}

HUX_TEST(several_peers_at_once_do_not_get_each_others_data) {
  /* One engine serving several peers concurrently is the shape a server
   * takes, and nothing had covered it: every other test uses one peer, or
   * several in sequence. What could go wrong is silent -- a request landing
   * in the right-sized region belonging to the wrong peer reports success
   * and corrupts somebody else's memory. */
  constexpr int kPeers = 3;
  constexpr uint64_t kSpan = 64 << 10;

  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread;
  MockConfig mc;
  mc.move_data = true; /* so the bytes can be checked, not just the counters */

  auto provider = std::make_shared<MockProvider>(mc);
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &engine), Status::kOk);

  /* A distinct source and destination per peer, each filled with a value
   * only that peer should ever produce. */
  std::vector<std::vector<uint8_t>> src(kPeers), dst(kPeers);
  std::vector<MemoryRegionPtr> src_reg(kPeers), dst_reg(kPeers);
  std::vector<PeerPtr> peers(kPeers);
  std::vector<RemoteRegionPtr> remotes(kPeers);

  for (int i = 0; i < kPeers; ++i) {
    src[i].assign(kSpan, static_cast<uint8_t>(0xA0 + i));
    dst[i].assign(kSpan, 0);
    CHECK_STATUS(engine->register_memory(src[i].data(), kSpan,
                                         AccessFlags::kRemoteRead, &src_reg[i]),
                 Status::kOk);
    CHECK_STATUS(engine->register_memory(dst[i].data(), kSpan,
                                         AccessFlags::kLocalWrite, &dst_reg[i]),
                 Status::kOk);
    std::vector<uint8_t> meta, desc;
    CHECK_STATUS(engine->local_metadata(&meta), Status::kOk);
    CHECK_STATUS(engine->add_peer(meta, &peers[i]), Status::kOk);
    CHECK_STATUS(src_reg[i]->export_descriptor(&desc), Status::kOk);
    CHECK_STATUS(peers[i]->import_region(desc, &remotes[i]), Status::kOk);
  }
  /* Distinct peers, not the same one handed back three times. */
  CHECK(peers[0]->id() != peers[1]->id() && peers[1]->id() != peers[2]->id());

  constexpr int kPerPeer = 30;
  std::atomic<int> submitted{0};
  std::vector<std::thread> threads;
  std::vector<std::vector<RequestPtr>> issued(kPeers);
  std::mutex mu;

  for (int i = 0; i < kPeers; ++i) {
    threads.emplace_back([&, i] {
      for (int n = 0; n < kPerPeer; ++n) {
        RegionView lv, rv;
        if (dst_reg[i]->view(0, kSpan, &lv) != Status::kOk) continue;
        if (remotes[i]->view(0, kSpan, &rv) != Status::kOk) continue;
        RequestPtr r;
        /* Offered again while the engine says its queue is full. kWouldBlock
         * is not a failure: nothing was accepted and nothing was sent, and
         * the contract is to retry. Skipping instead made this test assume
         * every submission is taken, which is only true when the machine is
         * idle -- under contention it submitted fewer than it counted on and
         * failed on the total, about eight times in a hundred. */
        Status s = Status::kWouldBlock;
        for (int tries = 0; tries < 10000; ++tries) {
          s = engine->read(peers[i].get(), lv, rv, {}, &r);
          if (s != Status::kWouldBlock) break;
          std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (s != Status::kOk) continue;
        ++submitted;
        std::lock_guard<std::mutex> g(mu);
        issued[i].push_back(std::move(r));
      }
    });
  }
  for (auto& t : threads) t.join();

  for (int i = 0; i < kPeers; ++i)
    for (auto& r : issued[i]) CHECK_STATUS(r->wait(10000), Status::kOk);

  /* Each destination holds its own peer's value and nobody else's. */
  for (int i = 0; i < kPeers; ++i) {
    uint8_t const mine = static_cast<uint8_t>(0xA0 + i);
    bool clean = true;
    for (uint64_t b = 0; b < kSpan; ++b)
      if (dst[i][b] != mine) {
        clean = false;
        break;
      }
    CHECK(clean);
  }
  CHECK_EQ(submitted.load(), kPeers * kPerPeer);

  EngineStats const st = engine->stats();
  /* Against what was actually submitted, not against the number the loop
   * intended. The two are the same now that back-pressure is retried, and
   * comparing to the intention is how a retry that quietly gave up would go
   * unnoticed. */
  CHECK_EQ(st.requests_succeeded, static_cast<uint64_t>(submitted.load()));
  CHECK_EQ(st.requests_failed, uint64_t{0});
}
