/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * What a ready handoff names, for a write in several segments.
 *
 * A regression. A vector write sent one handoff: the first segment's region
 * and offset, with the length of every segment added together. Two segments
 * into two regions were announced as one range of the first region, which
 * took in bytes nobody wrote and said nothing of the second region at all.
 * Now there is one handoff per range written, with ranges that follow on in
 * the same region merged. The mock delivers control messages back to the
 * engine that sent them, so one engine is both ends. */
#include <algorithm>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

struct Setup {
  std::shared_ptr<MockProvider> provider =
      std::make_shared<MockProvider>(MockConfig{});
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src = std::vector<uint8_t>(8192, 9);
  std::vector<uint8_t> a = std::vector<uint8_t>(8192, 0);
  std::vector<uint8_t> b = std::vector<uint8_t>(8192, 0);
  MemoryRegionPtr rsrc, ra, rb;
  PeerPtr peer;
  RemoteRegionPtr remote_a, remote_b;

  bool make() {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
    AccessFlags const target =
        AccessFlags::kLocalWrite | AccessFlags::kRemoteWrite;
    if (engine->register_memory(src.data(), src.size(), AccessFlags::kLocalRead,
                                &rsrc) != Status::kOk ||
        engine->register_memory(a.data(), a.size(), target, &ra) !=
            Status::kOk ||
        engine->register_memory(b.data(), b.size(), target, &rb) != Status::kOk)
      return false;
    std::vector<uint8_t> meta, da, db;
    if (engine->local_metadata(&meta) != Status::kOk ||
        engine->add_peer(meta, &peer) != Status::kOk ||
        ra->export_descriptor(&da) != Status::kOk ||
        rb->export_descriptor(&db) != Status::kOk)
      return false;
    return peer->import_region(da, &remote_a) == Status::kOk &&
           peer->import_region(db, &remote_b) == Status::kOk;
  }

  /* Writes each (region, offset, length) from the source and returns the
   * ready events that came back, in order of offset within region. */
  std::vector<ReadyEventPtr> writev(
      std::vector<std::pair<RemoteRegionPtr, Span>> const& segs,
      TransferOptions const& opts = {}) {
    std::vector<RegionView> locals, remotes;
    uint64_t at = 0;
    for (auto const& s : segs) {
      RegionView lv, rv;
      rsrc->view(at, s.second.length, &lv);
      s.first->view(s.second.offset, s.second.length, &rv);
      locals.push_back(lv);
      remotes.push_back(rv);
      at += s.second.length;
    }
    RequestPtr req;
    if (engine->writev(peer.get(), locals, remotes, opts, &req) != Status::kOk)
      return {};
    std::vector<RequestPtr> done;
    for (int i = 0; i < 100 && !is_terminal(req->state()); ++i)
      engine->poll_completions(16, &done);
    std::vector<ReadyEventPtr> ready, got;
    for (int i = 0; i < 10; ++i) {
      engine->poll_ready_events(16, &got);
      ready.insert(ready.end(), got.begin(), got.end());
    }
    std::sort(ready.begin(), ready.end(),
              [](ReadyEventPtr const& x, ReadyEventPtr const& y) {
                return x->region() != y->region()
                           ? x->region() < y->region()
                           : x->span().offset < y->span().offset;
              });
    return ready;
  }
};

}  // namespace

HUX_TEST(a_write_into_two_regions_names_both) {
  Setup s;
  CHECK(s.make());
  auto ready =
      s.writev({{s.remote_a, Span{100, 1000}}, {s.remote_b, Span{4096, 2000}}});
  CHECK_EQ(ready.size(), size_t(2));
  if (ready.size() != 2) return;
  CHECK_EQ(ready[0]->region(), s.ra->id());
  CHECK_EQ(ready[0]->span().offset, uint64_t(100));
  CHECK_EQ(ready[0]->span().length, uint64_t(1000));
  CHECK_EQ(ready[1]->region(), s.rb->id());
  CHECK_EQ(ready[1]->span().offset, uint64_t(4096));
  CHECK_EQ(ready[1]->span().length, uint64_t(2000));
}

HUX_TEST(ranges_apart_in_one_region_are_named_apart) {
  Setup s;
  CHECK(s.make());
  auto ready =
      s.writev({{s.remote_a, Span{0, 512}}, {s.remote_a, Span{4096, 512}}});
  CHECK_EQ(ready.size(), size_t(2));
  if (ready.size() != 2) return;
  CHECK_EQ(ready[0]->span().offset, uint64_t(0));
  CHECK_EQ(ready[0]->span().length, uint64_t(512));
  CHECK_EQ(ready[1]->span().offset, uint64_t(4096));
  CHECK_EQ(ready[1]->span().length, uint64_t(512));
}

HUX_TEST(ranges_that_follow_on_are_named_once) {
  Setup s;
  CHECK(s.make());
  auto ready =
      s.writev({{s.remote_a, Span{0, 512}}, {s.remote_a, Span{512, 1024}}});
  CHECK_EQ(ready.size(), size_t(1));
  if (ready.size() != 1) return;
  CHECK_EQ(ready[0]->span().offset, uint64_t(0));
  CHECK_EQ(ready[0]->span().length, uint64_t(1536));
}

HUX_TEST(a_write_without_a_handoff_tells_the_peer_nothing) {
  /* For a caller that signals arrival its own way: the bytes land, and no
   * handoff is sent -- the socket write it costs is most of a small write. */
  Setup s;
  CHECK(s.make());
  TransferOptions quiet;
  quiet.ready_handoff = false;
  auto ready = s.writev({{s.remote_a, Span{0, 512}}}, quiet);
  CHECK_EQ(ready.size(), size_t(0));
  CHECK_EQ(s.engine->stats().ready_handoffs_sent, uint64_t(0));
  CHECK(std::all_of(s.a.begin(), s.a.begin() + 512,
                    [](uint8_t v) { return v == 9; }));

  /* And the default still sends one. */
  ready = s.writev({{s.remote_b, Span{0, 512}}});
  CHECK_EQ(ready.size(), size_t(1));
  CHECK_EQ(s.engine->stats().ready_handoffs_sent, uint64_t(1));
}
