/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Region identifiers are handed out per engine, starting at one. Two peers
 * therefore both export a region numbered one, and an engine talking to both
 * has to keep them apart. Nothing here is about a peer misbehaving: two
 * ordinary peers, each numbering its own regions from the beginning. */
#include <cstring>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/local/local_provider.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

constexpr uint64_t kSpan = 4096;

struct Side {
  std::shared_ptr<MockProvider> prov;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> buf;
  MemoryRegionPtr reg;

  bool make(uint8_t fill) {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    MockConfig mc;
    mc.move_data = true;
    prov = std::make_shared<MockProvider>(mc);
    if (make_engine(cfg, nullptr, prov, &engine) != Status::kOk) return false;
    buf.assign(kSpan, fill);
    return engine->register_memory(
               buf.data(), kSpan,
               AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
               &reg) == Status::kOk;
  }
};

}  // namespace

HUX_TEST(two_peers_number_their_first_region_the_same) {
  /* The precondition, stated as a check rather than assumed: if this ever
   * stops being true the test below stops testing anything. */
  Side b, c;
  CHECK(b.make(0xBB));
  CHECK(c.make(0xCC));
  CHECK_EQ(b.reg->id(), c.reg->id());
}

HUX_TEST(a_read_from_one_peer_does_not_return_anothers_bytes) {
  /* Two peers, each exporting its own region one. A reads from the first.
   * Getting the second peer's bytes is silent: the request succeeds, the
   * length is right, and only the contents say anything is wrong. */
  Side b, c;
  CHECK(b.make(0xBB));
  CHECK(c.make(0xCC));

  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  MockConfig mc;
  mc.move_data = true;
  auto prov = std::make_shared<MockProvider>(mc);
  std::unique_ptr<Engine> a;
  CHECK_STATUS(make_engine(cfg, nullptr, prov, &a), Status::kOk);

  std::vector<uint8_t> into(kSpan, 0x00);
  MemoryRegionPtr into_reg;
  CHECK_STATUS(a->register_memory(into.data(), kSpan, AccessFlags::kLocalWrite,
                                  &into_reg),
               Status::kOk);

  PeerPtr peer_b, peer_c;
  RemoteRegionPtr rem_b, rem_c;
  std::vector<uint8_t> meta, desc;

  CHECK_STATUS(b.engine->local_metadata(&meta), Status::kOk);
  CHECK_STATUS(a->add_peer(meta, &peer_b), Status::kOk);
  CHECK_STATUS(b.reg->export_descriptor(&desc), Status::kOk);
  CHECK_STATUS(peer_b->import_region(desc, &rem_b), Status::kOk);

  CHECK_STATUS(c.engine->local_metadata(&meta), Status::kOk);
  CHECK_STATUS(a->add_peer(meta, &peer_c), Status::kOk);
  CHECK_STATUS(c.reg->export_descriptor(&desc), Status::kOk);
  CHECK_STATUS(peer_c->import_region(desc, &rem_c), Status::kOk);

  /* Importing the second must not have displaced the first. */
  RegionView lv, rv;
  CHECK_STATUS(into_reg->view(0, kSpan, &lv), Status::kOk);
  CHECK_STATUS(rem_b->view(0, kSpan, &rv), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(a->read(peer_b.get(), lv, rv, {}, &req), Status::kOk);
  std::vector<RequestPtr> done;
  for (int i = 0; i < 200 && !is_terminal(req->state()); ++i)
    a->poll_completions(16, &done);
  CHECK_STATUS(req->wait(0), Status::kOk);

  bool all_b = true;
  for (uint64_t i = 0; i < kSpan; ++i)
    if (into[i] != 0xBB) all_b = false;
  CHECK(all_b);
}

HUX_TEST(a_descriptor_from_before_a_restart_is_refused_not_used) {
  /* A peer goes away and comes back. The application still holds the
   * descriptor its previous incarnation exported, and the new one numbers
   * its regions from one again, so the descriptor looks current. Using it
   * would read memory that has been freed or reused.
   *
   * The local path is the one to test this on, because it has a registry
   * standing in for what a protection domain does across a network. The mock
   * dereferences whatever address it is given and would prove nothing. */
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;

  std::shared_ptr<LocalProvider> pa;
  std::unique_ptr<Engine> a;
  CHECK_STATUS(LocalProvider::create(&pa), Status::kOk);
  CHECK_STATUS(make_engine(cfg, nullptr, pa, &a), Status::kOk);

  std::vector<uint8_t> into(kSpan, 0x00);
  MemoryRegionPtr into_reg;
  CHECK_STATUS(a->register_memory(into.data(), kSpan, AccessFlags::kLocalWrite,
                                  &into_reg),
               Status::kOk);

  std::vector<uint8_t> stale_desc;
  {
    std::shared_ptr<LocalProvider> pb;
    std::unique_ptr<Engine> b;
    CHECK_STATUS(LocalProvider::create(&pb), Status::kOk);
    CHECK_STATUS(make_engine(cfg, nullptr, pb, &b), Status::kOk);
    pa->pair_with(pb);
    pb->pair_with(pa);

    std::vector<uint8_t> buf(kSpan, 0xBB);
    MemoryRegionPtr reg;
    CHECK_STATUS(
        b->register_memory(buf.data(), kSpan, AccessFlags::kRemoteRead, &reg),
        Status::kOk);
    CHECK_STATUS(reg->export_descriptor(&stale_desc), Status::kOk);
    /* Everything this peer had goes with it, buffer included. */
  }

  /* The replacement: a fresh engine numbering its regions from one again. */
  std::shared_ptr<LocalProvider> pb2;
  std::unique_ptr<Engine> b2;
  CHECK_STATUS(LocalProvider::create(&pb2), Status::kOk);
  CHECK_STATUS(make_engine(cfg, nullptr, pb2, &b2), Status::kOk);
  pa->pair_with(pb2);
  pb2->pair_with(pa);

  std::vector<uint8_t> buf2(kSpan, 0xCC);
  MemoryRegionPtr reg2;
  CHECK_STATUS(
      b2->register_memory(buf2.data(), kSpan, AccessFlags::kRemoteRead, &reg2),
      Status::kOk);
  /* The precondition again: the replacement's first region carries the same
   * id as the one that went away. */
  CHECK_EQ(reg2->id(), RegionId{1});

  std::vector<uint8_t> meta;
  PeerPtr peer2;
  CHECK_STATUS(b2->local_metadata(&meta), Status::kOk);
  CHECK_STATUS(a->add_peer(meta, &peer2), Status::kOk);

  /* At the door, and saying which door. Before the descriptor named its
   * origin this import succeeded and the transfer failed several calls
   * later with invalid_argument, which describes the argument rather than
   * the peer. */
  RemoteRegionPtr stale;
  CHECK_STATUS(peer2->import_region(stale_desc, &stale),
               Status::kStaleGeneration);

  /* Nothing was written into the destination. */
  bool untouched = true;
  for (uint64_t i = 0; i < kSpan; ++i)
    if (into[i] != 0x00) untouched = false;
  CHECK(untouched);
}

HUX_TEST(a_descriptor_belonging_to_one_peer_is_refused_by_another) {
  /* The same check, without a restart: two peers that are both alive. An
   * application holding several peers can pair a descriptor with the wrong
   * one, and every peer has a region 1 for it to land on. */
  Side b, c;
  CHECK(b.make(0xBB));
  CHECK(c.make(0xCC));

  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  MockConfig mc;
  mc.move_data = true;
  auto prov = std::make_shared<MockProvider>(mc);
  std::unique_ptr<Engine> a;
  CHECK_STATUS(make_engine(cfg, nullptr, prov, &a), Status::kOk);

  PeerPtr peer_b, peer_c;
  std::vector<uint8_t> meta, desc_b;
  CHECK_STATUS(b.engine->local_metadata(&meta), Status::kOk);
  CHECK_STATUS(a->add_peer(meta, &peer_b), Status::kOk);
  CHECK_STATUS(c.engine->local_metadata(&meta), Status::kOk);
  CHECK_STATUS(a->add_peer(meta, &peer_c), Status::kOk);

  CHECK_STATUS(b.reg->export_descriptor(&desc_b), Status::kOk);

  RemoteRegionPtr r;
  /* Into the peer it belongs to: fine. */
  CHECK_STATUS(peer_b->import_region(desc_b, &r), Status::kOk);
  /* Into the other one: refused. */
  CHECK_STATUS(peer_c->import_region(desc_b, &r), Status::kStaleGeneration);
}
