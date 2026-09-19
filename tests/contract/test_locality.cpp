/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Deciding where a peer is. The answer selects the path, so getting it wrong
 * in one direction costs speed and in the other corrupts memory: an engine
 * that believes a remote peer is local would hand a NIC address to memcpy. */
#include <algorithm>
#include <vector>

#include "control/identity.h"
#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {
Identity make(uint64_t host, uint64_t proc, uint64_t engine) {
  Identity i;
  i.host = host;
  i.process = proc;
  i.engine = engine;
  return i;
}
}  // namespace

HUX_TEST(locality_is_decided_by_host_then_process_then_engine) {
  CHECK(locality_of(make(1, 10, 100), make(2, 10, 100)) == Locality::kRemote);
  CHECK(locality_of(make(1, 10, 100), make(1, 20, 100)) == Locality::kSameHost);
  CHECK(locality_of(make(1, 10, 100), make(1, 10, 200)) ==
        Locality::kSameProcess);
  CHECK(locality_of(make(1, 10, 100), make(1, 10, 100)) ==
        Locality::kSameEngine);
}

HUX_TEST(a_different_host_outranks_a_matching_process_id) {
  /* Process ids repeat across machines. Comparing them first would call two
   * unrelated processes on different hosts the same engine, and hand a remote
   * address to a local copy. */
  CHECK(locality_of(make(1, 4242, 1), make(2, 4242, 1)) == Locality::kRemote);
}

HUX_TEST(identity_round_trips) {
  Identity const id = make(0xAABBCCDDEEFF0011ull, 4242, 7);
  std::vector<uint8_t> buf;
  encode_identity(id, &buf);
  CHECK_EQ(buf.size(), kIdentityBytes);

  Identity got;
  CHECK_STATUS(decode_identity(buf, 0, &got), Status::kOk);
  CHECK_EQ(got.host, id.host);
  CHECK_EQ(got.process, id.process);
  CHECK_EQ(got.engine, id.engine);
}

HUX_TEST(a_truncated_identity_is_refused) {
  std::vector<uint8_t> buf;
  encode_identity(make(1, 2, 3), &buf);
  buf.pop_back();
  Identity got;
  CHECK_STATUS(decode_identity(buf, 0, &got), Status::kInvalidArgument);
}

HUX_TEST(two_engines_in_one_process_see_each_other_as_local) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  auto pa = std::make_shared<MockProvider>(MockConfig{});
  auto pb = std::make_shared<MockProvider>(MockConfig{});
  std::unique_ptr<Engine> a, b;
  CHECK_STATUS(make_engine(cfg, nullptr, pa, &a), Status::kOk);
  CHECK_STATUS(make_engine(cfg, nullptr, pb, &b), Status::kOk);

  std::vector<uint8_t> meta;
  CHECK_STATUS(b->local_metadata(&meta), Status::kOk);
  /* The identity sits in front of whatever the provider needs. */
  CHECK(meta.size() > kIdentityBytes);

  PeerPtr peer;
  CHECK_STATUS(a->add_peer(meta, &peer), Status::kOk);
  CHECK(peer->caps().place == PeerPlace::kSameProcess);
  /* And the path is the network one, because that is the only transport this
   * engine was given. Being next door does not make a NIC transfer local, and
   * reporting otherwise would hide exactly the fallback a caller is asking
   * about. */
  CHECK(peer->caps().path == PathKind::kRdma);
}

HUX_TEST(a_peer_on_another_host_is_reported_as_remote) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  auto provider = std::make_shared<MockProvider>(MockConfig{});
  std::unique_ptr<Engine> e;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &e), Status::kOk);

  std::vector<uint8_t> meta;
  CHECK_STATUS(e->local_metadata(&meta), Status::kOk);
  /* Rewrite the host in place, which is what a peer elsewhere would send. */
  std::vector<uint8_t> foreign;
  encode_identity(make(0xFFFFFFFFull, 1, 1), &foreign);
  std::copy(foreign.begin(), foreign.end(), meta.begin());

  PeerPtr peer;
  CHECK_STATUS(e->add_peer(meta, &peer), Status::kOk);
  CHECK(peer->caps().path == PathKind::kRdma);
}

HUX_TEST(metadata_without_an_identity_is_treated_as_remote) {
  /* Older metadata, or a peer that only supplies what its provider needs,
   * carries no identity. Assuming local would be the dangerous guess -- a
   * remote address handed to a local copy -- so the absence of information
   * means remote, which at worst costs speed. */
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  auto provider = std::make_shared<MockProvider>(MockConfig{});
  std::unique_ptr<Engine> e;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &e), Status::kOk);

  std::vector<uint8_t> bare = {'m', 'o', 'c', 'k'}; /* provider metadata only */
  PeerPtr peer;
  CHECK_STATUS(e->add_peer(bare, &peer), Status::kOk);
  CHECK(peer->caps().path == PathKind::kRdma);
}
