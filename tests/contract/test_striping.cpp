/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * One transfer carried by several adapters at once.
 *
 * Worth doing only because the adapters do not share a bottleneck -- measured
 * on this hardware at 75.4 Gb/s against 51.6 for the better one alone -- and
 * worth weighting because they are rarely equal: the two here differ by
 * 2.14x, and an even split would be held to the slower one and finish below
 * the faster one used alone.
 *
 * What is checked is the split and the bytes. A transfer that lands entirely
 * on one lane still succeeds and still carries the right data; only the
 * counters say it was never split. */
#include <cstring>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

constexpr uint64_t kChunk = 64 << 10;

/* An engine reached over two transports of one family, as a host with two
 * adapters would be. */
struct TwoLane {
  std::shared_ptr<MockProvider> fast, slow;
  std::unique_ptr<Engine> engine;

  bool make(double fast_capacity, double slow_capacity) {
    MockConfig a;
    a.name = "mock";
    a.relative_capacity = fast_capacity;
    a.move_data = true;
    MockConfig b = a;
    b.name = "mock#1";
    b.relative_capacity = slow_capacity;

    fast = std::make_shared<MockProvider>(a);
    slow = std::make_shared<MockProvider>(b);
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    cfg.chunk_bytes = kChunk;
    std::vector<TransportProviderPtr> provs{fast, slow};
    return make_engine(cfg, nullptr, provs, &engine) == Status::kOk;
  }
};

Status drive(Engine* e, RequestPtr const& r) {
  std::vector<RequestPtr> done;
  for (int i = 0; i < 2000 && !is_terminal(r->state()); ++i)
    e->poll_completions(32, &done);
  return r->wait(0);
}

}  // namespace

HUX_TEST(a_peer_offering_two_siblings_is_reached_over_both) {
  TwoLane t;
  CHECK(t.make(1.0, 1.0));
  std::vector<uint8_t> buf(kChunk, 0);
  MemoryRegionPtr reg;
  CHECK_STATUS(t.engine->register_memory(buf.data(), kChunk,
                                         AccessFlags::kRemoteRead, &reg),
               Status::kOk);
  std::vector<uint8_t> meta;
  CHECK_STATUS(t.engine->local_metadata(&meta), Status::kOk);
  PeerPtr peer;
  CHECK_STATUS(t.engine->add_peer(meta, &peer), Status::kOk);
  CHECK_EQ(peer->caps().lane_count, 2u);
}

HUX_TEST(a_transfer_is_split_between_them_in_proportion) {
  /* Eight chunks across lanes weighted 3 to 1: six and two, not four each.
   * With equal weights it would be four and four, which is the wrong answer
   * whenever the adapters are not equal. */
  TwoLane t;
  CHECK(t.make(3.0, 1.0));

  constexpr uint64_t kSpan = 8 * kChunk;
  std::vector<uint8_t> src(kSpan), dst(kSpan, 0);
  for (uint64_t i = 0; i < kSpan; ++i) src[i] = static_cast<uint8_t>(i * 7 + 1);

  MemoryRegionPtr src_reg, dst_reg;
  CHECK_STATUS(t.engine->register_memory(src.data(), kSpan,
                                         AccessFlags::kRemoteRead, &src_reg),
               Status::kOk);
  CHECK_STATUS(t.engine->register_memory(dst.data(), kSpan,
                                         AccessFlags::kLocalWrite, &dst_reg),
               Status::kOk);

  std::vector<uint8_t> meta, desc;
  CHECK_STATUS(t.engine->local_metadata(&meta), Status::kOk);
  PeerPtr peer;
  CHECK_STATUS(t.engine->add_peer(meta, &peer), Status::kOk);
  CHECK_STATUS(src_reg->export_descriptor(&desc), Status::kOk);
  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kOk);

  uint64_t const fast_before = t.fast->stats().subops_posted;
  uint64_t const slow_before = t.slow->stats().subops_posted;

  RegionView lv, rv;
  CHECK_STATUS(dst_reg->view(0, kSpan, &lv), Status::kOk);
  CHECK_STATUS(remote->view(0, kSpan, &rv), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(t.engine->read(peer.get(), lv, rv, {}, &req), Status::kOk);
  CHECK_STATUS(drive(t.engine.get(), req), Status::kOk);

  uint64_t const on_fast = t.fast->stats().subops_posted - fast_before;
  uint64_t const on_slow = t.slow->stats().subops_posted - slow_before;
  CHECK_EQ(on_fast + on_slow, 8u);
  /* Both carried something: a split that put everything on one lane would
   * pass every other check in this file. */
  CHECK(on_slow > 0);
  CHECK(on_fast > on_slow);
  CHECK_EQ(on_fast, 6u);

  /* And the bytes are right, which is the part a split can quietly break:
   * every chunk has to reach the offset it was cut from, whichever lane
   * carried it. */
  CHECK(std::memcmp(dst.data(), src.data(), kSpan) == 0);
}

HUX_TEST(an_even_split_is_what_weighting_avoids) {
  /* The control for the test above: with nothing to tell them apart the
   * chunks divide evenly, so the six-and-two result is the weights doing
   * something rather than an artefact of the order chunks are made in. */
  TwoLane t;
  CHECK(t.make(1.0, 1.0));

  constexpr uint64_t kSpan = 8 * kChunk;
  std::vector<uint8_t> src(kSpan, 0xAB), dst(kSpan, 0);
  MemoryRegionPtr src_reg, dst_reg;
  CHECK_STATUS(t.engine->register_memory(src.data(), kSpan,
                                         AccessFlags::kRemoteRead, &src_reg),
               Status::kOk);
  CHECK_STATUS(t.engine->register_memory(dst.data(), kSpan,
                                         AccessFlags::kLocalWrite, &dst_reg),
               Status::kOk);
  std::vector<uint8_t> meta, desc;
  CHECK_STATUS(t.engine->local_metadata(&meta), Status::kOk);
  PeerPtr peer;
  CHECK_STATUS(t.engine->add_peer(meta, &peer), Status::kOk);
  CHECK_STATUS(src_reg->export_descriptor(&desc), Status::kOk);
  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kOk);

  uint64_t const fast_before = t.fast->stats().subops_posted;
  uint64_t const slow_before = t.slow->stats().subops_posted;
  RegionView lv, rv;
  CHECK_STATUS(dst_reg->view(0, kSpan, &lv), Status::kOk);
  CHECK_STATUS(remote->view(0, kSpan, &rv), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(t.engine->read(peer.get(), lv, rv, {}, &req), Status::kOk);
  CHECK_STATUS(drive(t.engine.get(), req), Status::kOk);

  CHECK_EQ(t.fast->stats().subops_posted - fast_before, 4u);
  CHECK_EQ(t.slow->stats().subops_posted - slow_before, 4u);
  CHECK(std::memcmp(dst.data(), src.data(), kSpan) == 0);
}

HUX_TEST(a_transfer_smaller_than_a_chunk_stays_on_one_lane) {
  /* Splitting below the chunk would trade a round trip for balance that a
   * single chunk cannot use. */
  TwoLane t;
  CHECK(t.make(1.0, 1.0));
  std::vector<uint8_t> src(1024, 0x5A), dst(1024, 0);
  MemoryRegionPtr src_reg, dst_reg;
  CHECK_STATUS(t.engine->register_memory(src.data(), 1024,
                                         AccessFlags::kRemoteRead, &src_reg),
               Status::kOk);
  CHECK_STATUS(t.engine->register_memory(dst.data(), 1024,
                                         AccessFlags::kLocalWrite, &dst_reg),
               Status::kOk);
  std::vector<uint8_t> meta, desc;
  CHECK_STATUS(t.engine->local_metadata(&meta), Status::kOk);
  PeerPtr peer;
  CHECK_STATUS(t.engine->add_peer(meta, &peer), Status::kOk);
  CHECK_STATUS(src_reg->export_descriptor(&desc), Status::kOk);
  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kOk);

  uint64_t const before =
      t.fast->stats().subops_posted + t.slow->stats().subops_posted;
  RegionView lv, rv;
  CHECK_STATUS(dst_reg->view(0, 1024, &lv), Status::kOk);
  CHECK_STATUS(remote->view(0, 1024, &rv), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(t.engine->read(peer.get(), lv, rv, {}, &req), Status::kOk);
  CHECK_STATUS(drive(t.engine.get(), req), Status::kOk);
  CHECK_EQ(
      t.fast->stats().subops_posted + t.slow->stats().subops_posted - before,
      1u);
  CHECK(std::memcmp(dst.data(), src.data(), 1024) == 0);
}
