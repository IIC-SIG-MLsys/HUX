/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * What has to remain true after something breaks. A transport that only
 * behaves while everything works is the easy half; these are the cases where
 * a caller has to be able to tell what happened and what it may still touch. */
#include <vector>

#include "control/control_message.h"
#include "core/factory.h"
#include "core/region_impl.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

struct FailFixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src, dst;
  MemoryRegionPtr src_region, dst_region;
  PeerPtr peer;
  RemoteRegionPtr remote;

  bool setup(MockConfig mc = {}) {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    provider = std::make_shared<MockProvider>(mc);
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
    src.assign(4096, 0xAB);
    dst.assign(4096, 0);
    /* Exported for both, since the failure tests below write to it as well
     * as reading from it. */
    if (engine->register_memory(
            src.data(), 4096,
            AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
            &src_region) != Status::kOk)
      return false;
    if (engine->register_memory(dst.data(), 4096, AccessFlags::kLocalWrite,
                                &dst_region) != Status::kOk)
      return false;
    std::vector<uint8_t> meta, desc;
    engine->local_metadata(&meta);
    if (engine->add_peer(meta, &peer) != Status::kOk) return false;
    src_region->export_descriptor(&desc);
    return peer->import_region(desc, &remote) == Status::kOk;
  }

  void views(RegionView* l, RegionView* r) {
    dst_region->view(0, 4096, l);
    remote->view(0, 4096, r);
  }
};

}  // namespace

HUX_TEST(removing_a_peer_marks_it_disconnected) {
  FailFixture f;
  CHECK(f.setup());
  CHECK_EQ(f.peer->connected(), true);
  Epoch const before = f.peer->epoch();

  CHECK_STATUS(f.engine->remove_peer(f.peer), Status::kOk);

  /* A caller holding the handle has to be able to see that it is gone. */
  CHECK_EQ(f.peer->connected(), false);
  /* The epoch moves so a later connection cannot be mistaken for this one,
   * and requests from before cannot be carried over to it. */
  CHECK(f.peer->epoch() > before);
}

HUX_TEST(a_disconnected_peer_refuses_new_transfers) {
  FailFixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  CHECK_STATUS(f.engine->remove_peer(f.peer), Status::kOk);

  RequestPtr req;
  /* Refused up front, with a reason. Accepting it and failing later would
   * leave the caller unable to tell a broken connection from a broken
   * transfer. */
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, {}, &req),
               Status::kPeerDisconnected);
  CHECK_STATUS(f.engine->notify(f.peer.get(), {1, 2, 3}, &req),
               Status::kPeerDisconnected);
}

HUX_TEST(deregistering_tells_the_peer_to_stop_using_the_region) {
  /* Otherwise the peer keeps a descriptor for memory that is no longer
   * registered, and submits against it until the hardware refuses -- long
   * after the local side believed it had taken the region back. */
  FailFixture f;
  CHECK(f.setup());

  CHECK(f.remote->valid());
  CHECK_STATUS(f.engine->deregister_memory(f.src_region), Status::kOk);

  /* The notice travels on the control channel; one pass delivers it. */
  std::vector<RequestPtr> done;
  for (int i = 0; i < 20 && f.remote->valid(); ++i)
    f.engine->poll_completions(8, &done);

  CHECK_EQ(f.remote->valid(), false);

  RegionView v;
  CHECK_STATUS(f.remote->view(0, 4096, &v), Status::kStaleGeneration);
}

HUX_TEST(an_invalidated_region_cannot_be_submitted_against) {
  FailFixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  std::static_pointer_cast<RemoteRegionImpl>(f.remote)->invalidate();

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, {}, &req),
               Status::kStaleGeneration);
}

HUX_TEST(close_reports_a_timeout_rather_than_abandoning_dma) {
  /* A close that returned ok while work was still outstanding would invite
   * the caller to free memory the NIC may still be reading. */
  MockConfig mc;
  mc.move_data = false;
  mc.never_complete = true; /* accepted, never finishes */
  FailFixture f;
  CHECK(f.setup(mc));
  RegionView lv, rv;
  f.views(&lv, &rv);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, {}, &req), Status::kOk);
  CHECK_STATUS(f.engine->close(50), Status::kTimeout);
}

HUX_TEST(in_flight_requests_reach_a_terminal_state_when_the_peer_goes) {
  /* Every request has to end somewhere. One left in flight forever holds its
   * regions and its connection, and nothing can be released.
   *
   * Reaching a terminal state is the requirement, not failing: removing a
   * peer after the data has already moved leaves a request that legitimately
   * succeeded. */
  FailFixture f;
  CHECK(f.setup());
  RegionView lv, rv;
  f.views(&lv, &rv);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, {}, &req), Status::kOk);
  CHECK_STATUS(f.engine->remove_peer(f.peer), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 200 && !is_terminal(req->state()); ++i)
    f.engine->poll_completions(8, &done);

  CHECK(is_terminal(req->state()));
}

HUX_TEST(a_transport_failure_ends_the_request_with_a_reason) {
  /* When the transfer itself fails, the request has to say so and carry
   * whether the target may have been touched -- the caller cannot decide what
   * the remote side holds without it. */
  MockConfig mc;
  mc.fail_subops = true;
  mc.move_data = false;
  FailFixture f;
  CHECK(f.setup(mc));
  RegionView lv, rv;
  f.views(&lv, &rv);

  RequestPtr req;
  CHECK_STATUS(f.engine->write(f.peer.get(), lv, rv, {}, &req), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 200 && !is_terminal(req->state()); ++i)
    f.engine->poll_completions(8, &done);

  CHECK(req->state() == RequestState::kFailed);
  CHECK(!req->error().ok());
  CHECK(req->reached(Stage::kFailedSafe));
  CHECK(!req->reached(Stage::kTargetReady));
  CHECK_EQ(req->error().may_have_modified_target, true);
}

HUX_TEST(an_invalidation_for_another_generation_is_ignored) {
  /* Region ids are reused. A notice naming an older incarnation must not
   * retire the region that replaced it, or a live transfer would be refused
   * for a reason that no longer exists. */
  FailFixture f;
  CHECK(f.setup());
  CHECK(f.remote->valid());

  RegionInvalidateBody b;
  b.region = f.remote->id();
  b.generation = f.remote->generation() + 7; /* not this one */
  std::vector<uint8_t> payload;
  encode_region_invalidate(b, &payload);
  CHECK_STATUS(
      f.provider->send_control(
          nullptr, static_cast<uint16_t>(ControlType::kRegionInvalidate),
          payload),
      Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 20; ++i) f.engine->poll_completions(8, &done);

  CHECK_EQ(f.remote->valid(), true);
}

HUX_TEST(a_departed_peer_fails_what_was_in_flight_to_it) {
  /* A peer that exits between transfers leaves nothing to fail on its own:
   * the request was already submitted and its completions are simply never
   * coming. Without the engine noticing the connection has gone, it waits
   * for ever. */
  MockConfig cfg;
  cfg.never_complete = true;
  auto provider = std::make_shared<MockProvider>(cfg);
  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(ecfg, nullptr, provider, &engine), Status::kOk);

  std::vector<uint8_t> buf(8192, 0);
  MemoryRegionPtr local;
  CHECK_STATUS(engine->register_memory(buf.data(), buf.size(),
                                       AccessFlags::kRemoteRead, &local),
               Status::kOk);
  std::vector<uint8_t> desc;
  CHECK_STATUS(local->export_descriptor(&desc), Status::kOk);

  std::vector<uint8_t> meta;
  CHECK_STATUS(engine->local_metadata(&meta), Status::kOk);
  PeerPtr peer;
  CHECK_STATUS(engine->add_peer(meta, &peer), Status::kOk);
  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kOk);

  RegionView lv, rv;
  CHECK_STATUS(local->view(0, 4096, &lv), Status::kOk);
  CHECK_STATUS(remote->view(0, 4096, &rv), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(engine->read(peer.get(), lv, rv, {}, &req), Status::kOk);
  bool done = false;
  req->test(&done);
  CHECK(!done);
  CHECK(peer->connected());

  /* The peer goes. Nothing about the request changes by itself. */
  provider->retire_connections();
  std::vector<RequestPtr> finished;
  engine->poll_completions(8, &finished);

  req->test(&done);
  CHECK(done);
  CHECK_STATUS(req->error().status, Status::kPeerDisconnected);
  CHECK(!peer->connected());
}

HUX_TEST(a_departed_peer_says_a_write_may_have_landed) {
  /* Whether a write already posted reached the peer cannot be known after a
   * disconnect, and a target owner has to be told that rather than left to
   * assume either way. */
  MockConfig cfg;
  cfg.never_complete = true;
  auto provider = std::make_shared<MockProvider>(cfg);
  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(ecfg, nullptr, provider, &engine), Status::kOk);

  std::vector<uint8_t> buf(8192, 0);
  MemoryRegionPtr local;
  CHECK_STATUS(engine->register_memory(buf.data(), buf.size(),
                                       AccessFlags::kRemoteWrite, &local),
               Status::kOk);
  std::vector<uint8_t> desc;
  CHECK_STATUS(local->export_descriptor(&desc), Status::kOk);
  std::vector<uint8_t> meta;
  CHECK_STATUS(engine->local_metadata(&meta), Status::kOk);
  PeerPtr peer;
  CHECK_STATUS(engine->add_peer(meta, &peer), Status::kOk);
  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kOk);

  RegionView lv, rv;
  CHECK_STATUS(local->view(0, 4096, &lv), Status::kOk);
  CHECK_STATUS(remote->view(0, 4096, &rv), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(engine->write(peer.get(), lv, rv, {}, &req), Status::kOk);

  provider->retire_connections();
  std::vector<RequestPtr> finished;
  engine->poll_completions(8, &finished);

  CHECK_STATUS(req->error().status, Status::kPeerDisconnected);
  CHECK(req->error().may_have_modified_target);
}
