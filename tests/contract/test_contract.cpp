/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * M0 contract tests, covering the hardware-free core/mock column of the
 * roadmap's verification matrix. The cases deliberately target where earlier
 * implementations went wrong -- out-of-order completions, partial posts, an
 * lkey exported in place of an rkey, a timeout mistaken for a cancellation --
 * so those show up as failing tests instead of during a code read. */
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "control/identity.h"
#include "core/factory.h"
#include "core/region_impl.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

struct Fixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src;
  std::vector<uint8_t> dst;
  MemoryRegionPtr src_region;
  MemoryRegionPtr dst_region;
  PeerPtr peer;
  RemoteRegionPtr remote_src;

  /* Minimal working setup: two local buffers, one acting as the local region
   * and one as the peer's. The mock moves bytes within one address space, so
   * content can be verified directly. */
  bool setup(EngineConfig cfg = {}, MockConfig mock = {}, size_t bytes = 4096) {
    provider = std::make_shared<MockProvider>(mock);
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
    src.assign(bytes, 0);
    dst.assign(bytes, 0);
    for (size_t i = 0; i < bytes; ++i) src[i] = static_cast<uint8_t>(i * 7 + 1);

    if (engine->register_memory(
            src.data(), bytes,
            AccessFlags::kRemoteRead | AccessFlags::kLocalRead,
            &src_region) != Status::kOk)
      return false;
    if (engine->register_memory(
            dst.data(), bytes,
            AccessFlags::kLocalWrite | AccessFlags::kRemoteWrite,
            &dst_region) != Status::kOk)
      return false;
    std::vector<uint8_t> meta;
    if (engine->local_metadata(&meta) != Status::kOk) return false;
    if (engine->add_peer(meta, &peer) != Status::kOk) return false;
    std::vector<uint8_t> desc;
    if (src_region->export_descriptor(&desc) != Status::kOk) return false;
    if (peer->import_region(desc, &remote_src) != Status::kOk) return false;
    return true;
  }

  /* Drives the request to a terminal state; in explicit mode
   * poll_completions pumps progress itself. */
  Status drain(RequestPtr const& req, int64_t ms = 2000) {
    std::vector<RequestPtr> done;
    for (int i = 0; i < 2000; ++i) {
      engine->poll_completions(16, &done);
      bool fin = false;
      req->test(&fin);
      if (fin) return req->wait(ms);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Status::kTimeout;
  }
};

EngineConfig explicit_cfg() {
  EngineConfig c;
  c.progress = ProgressMode::kExplicit;
  return c;
}

}  // namespace

/* ---- Descriptor codec ---- */

HUX_TEST(descriptor_roundtrip) {
  RegionDescriptor d;
  d.region = 42;
  d.generation = 7;
  d.base = 0x7f0000001000ull;
  d.length = 1ull << 33; /* >4 GiB: catches a truncated 64-bit field. */
  d.remote_key = 0xdeadbeefull;
  d.device_kind = DeviceKind::kCambricon;
  d.device_index = 3;
  d.access = AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite;

  std::vector<uint8_t> buf;
  encode_descriptor(d, &buf);
  RegionDescriptor got;
  CHECK_STATUS(decode_descriptor(buf, &got), Status::kOk);
  CHECK_EQ(got.region, d.region);
  CHECK_EQ(got.generation, d.generation);
  CHECK_EQ(got.base, d.base);
  CHECK_EQ(got.length, d.length);
  CHECK_EQ(got.remote_key, d.remote_key);
  CHECK(got.device_kind == DeviceKind::kCambricon);
  CHECK_EQ(got.device_index, 3);
}

HUX_TEST(descriptor_rejects_incompatible_major) {
  RegionDescriptor d;
  std::vector<uint8_t> buf;
  encode_descriptor(d, &buf);
  buf[0] = static_cast<uint8_t>(kDescriptorMajor + 1);
  RegionDescriptor got;
  CHECK_STATUS(decode_descriptor(buf, &got), Status::kUnsupported);
}

HUX_TEST(descriptor_rejects_truncated) {
  RegionDescriptor d;
  std::vector<uint8_t> buf;
  encode_descriptor(d, &buf);
  buf.pop_back();
  RegionDescriptor got;
  CHECK_STATUS(decode_descriptor(buf, &got), Status::kInvalidArgument);
}

/* ---- Range checks ---- */

HUX_TEST(view_rejects_overflow) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView v;
  /* offset + length wraps. Comparing before adding catches it; adding first
   * lets it through. */
  CHECK_STATUS(f.src_region->view(0xffffffffffffff00ull, 0x200, &v),
               Status::kOutOfRange);
  CHECK_STATUS(f.src_region->view(4000, 1000, &v), Status::kOutOfRange);
  CHECK_STATUS(f.src_region->view(0, 4096, &v), Status::kOk);
}

/* ---- Basic transfers and data correctness ---- */

HUX_TEST(read_moves_data) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  CHECK(req->reached(Stage::kTargetReady) ||
        req->reached(Stage::kTransferComplete));
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

HUX_TEST(readv_pairs_segments) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  std::vector<RegionView> locals(2), remotes(2);
  CHECK_STATUS(f.dst_region->view(0, 1024, &locals[0]), Status::kOk);
  CHECK_STATUS(f.dst_region->view(2048, 1024, &locals[1]), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 1024, &remotes[0]), Status::kOk);
  CHECK_STATUS(f.remote_src->view(2048, 1024, &remotes[1]), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(f.engine->readv(f.peer.get(), locals, remotes, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 1024), 0);
  CHECK_EQ(std::memcmp(f.dst.data() + 2048, f.src.data() + 2048, 1024), 0);
  /* The untouched gap must stay zero, or a segment boundary is wrong. */
  for (size_t i = 1024; i < 2048; ++i) CHECK_EQ(f.dst[i], 0);
}

HUX_TEST(mismatched_segment_lengths_rejected) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  std::vector<RegionView> locals(1), remotes(1);
  CHECK_STATUS(f.dst_region->view(0, 1024, &locals[0]), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 512, &remotes[0]), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->readv(f.peer.get(), locals, remotes, {}, &req),
               Status::kInvalidArgument);
}

/* ---- Chunking ---- */

HUX_TEST(chunking_covers_whole_range) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 512; /* 4096 bytes becomes 8 chunks. */
  Fixture f;
  CHECK(f.setup(cfg));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  CHECK_EQ(f.provider->submitted_subops(), 8u);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

HUX_TEST(no_chunk_is_longer_than_a_transport_takes) {
  /* The engine split by chunk_bytes alone, whatever a transport said the
   * longest operation it takes was. */
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 4096;
  MockConfig mock;
  mock.max_segment_bytes = 1024;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  CHECK_EQ(f.provider->submitted_subops(), 4u);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

HUX_TEST(a_chunk_longer_than_a_work_request_carries_is_refused) {
  /* A work request carries a 32-bit length, so a 4 GiB chunk went out as
   * zero bytes and was reported as moved. */
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 1ull << 32;
  std::string why;
  CHECK_STATUS(cfg.validate(&why), Status::kInvalidArgument);
  CHECK(!why.empty());
  cfg.chunk_bytes = 0xffffffffull;
  CHECK_STATUS(cfg.validate(&why), Status::kOk);
}

HUX_TEST(metadata_cut_short_after_its_version_is_refused) {
  /* A regression. The count after the version was read before checking that
   * it was there: two bytes past the end of a blob cut at that point.
   * AddressSanitizer is what sees it; the answer was a refusal either way. */
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  std::vector<uint8_t> meta;
  CHECK_STATUS(f.engine->local_metadata(&meta), Status::kOk);
  for (size_t cut : {kIdentityBytes + 8, kIdentityBytes + 9}) {
    std::vector<uint8_t> shortened(meta.begin(), meta.begin() + cut);
    PeerPtr p;
    CHECK_STATUS(f.engine->add_peer(shortened, &p), Status::kInvalidArgument);
  }
}

HUX_TEST(a_write_into_a_region_exported_for_reading_is_refused) {
  /* A regression. Nothing checked what a region was exported for: over RDMA
   * the far adapter refused the write, as an error that fails every request
   * on the connection; over the local and IPC paths it simply landed. */
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->write(f.peer.get(), local, remote, {}, &req),
               Status::kInvalidArgument);
  CHECK(req == nullptr);
  CHECK_EQ(f.provider->submitted_subops(), 0u);

  /* Reading from it, which is what it was exported for, still works. */
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
}

/* ---- Out-of-order completions ---- */

HUX_TEST(out_of_order_completions_aggregate_correctly) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 256; /* 16 chunks. */
  MockConfig mock;
  mock.shuffle_completions = true;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  /* Order must not affect aggregation: success needs all 16 to report. */
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

/* ---- One CQ batch spanning several requests ---- */

HUX_TEST(batch_poll_does_not_drop_other_requests) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 1024;
  Fixture f;
  CHECK(f.setup(cfg));

  std::vector<RequestPtr> reqs;
  for (int i = 0; i < 4; ++i) {
    RegionView local, remote;
    CHECK_STATUS(f.dst_region->view(i * 1024, 1024, &local), Status::kOk);
    CHECK_STATUS(f.remote_src->view(i * 1024, 1024, &remote), Status::kOk);
    RequestPtr r;
    CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &r),
                 Status::kOk);
    reqs.push_back(r);
  }
  /* Returning early on a matching entry drops the other requests in the same
   * batch, which shows up as a request that never reaches a terminal state. */
  for (auto& r : reqs) CHECK_STATUS(f.drain(r), Status::kOk);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

/* ---- Partial submit ---- */

HUX_TEST(partial_submit_reports_and_keeps_accepted) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 512; /* 8 chunks. */
  MockConfig mock;
  mock.accept_limit = 3;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  Status s = f.engine->read(f.peer.get(), local, remote, {}, &req);
  CHECK_STATUS(s, Status::kOk); /* Partly accepted counts as accepted. */
  CHECK(req != nullptr);
  /* The accepted part keeps draining, and the error must carry
   * may_have_modified_target. */
  f.drain(req, 500);
  CHECK(!req->error().ok());
  CHECK_EQ(f.provider->submitted_subops(), 3u);
}

/* FailedSafe says local DMA has stopped. When submission gives up partway,
 * the sub-operations it already handed to the NIC are still reading the
 * source, so the request cannot be terminal yet -- a caller that frees its
 * buffer on seeing the failure would hand the NIC memory it no longer owns.
 * The request used to fail at submit time, before any of them completed. */
HUX_TEST(partial_submit_waits_while_the_nic_still_reads_the_source) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 512; /* 8 chunks. */
  MockConfig mock;
  mock.accept_limit = 3;
  mock.never_complete = true; /* the three stay outstanding */
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK(req != nullptr);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 50; ++i) f.engine->poll_completions(16, &done);
  bool finished = true;
  CHECK_STATUS(req->test(&finished), Status::kOk);
  CHECK(!finished);
  CHECK_STATUS(req->wait(20), Status::kTimeout);
}

/* And when they do complete, the request has to be released. The completion
 * path skips anything already terminal, so failing at submit time left the
 * entry in the in-flight table for good -- which the engine's own bound then
 * counts against every later request. */
HUX_TEST(partial_submit_releases_the_request) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 512;
  cfg.max_inflight_requests = 4;
  MockConfig mock;
  mock.accept_limit = 3;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);

  /* Three times the bound: if the failures are not released, admission stops
   * after four. */
  for (int i = 0; i < 12; ++i) {
    RequestPtr req;
    CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
                 Status::kOk);
    CHECK_STATUS(f.drain(req, 500), Status::kResourceExhausted);
    CHECK(!req->error().ok());
  }
  EngineStats const st = f.engine->stats();
  CHECK_EQ(st.requests_failed, uint64_t(12));
}

HUX_TEST(full_submit_rejection_has_no_side_effect) {
  EngineConfig cfg = explicit_cfg();
  MockConfig mock;
  mock.reject_all = true;
  mock.submit_status_on_partial = Status::kResourceExhausted;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  f.engine->read(f.peer.get(), local, remote, {}, &req);
  CHECK_EQ(f.provider->submitted_subops(), 0u);
}

HUX_TEST(a_transfer_of_nothing_is_refused_and_leaves_nothing_behind) {
  /* A regression. A zero-length transfer produced no sub-operation, so
   * nothing could ever end it: the call returned kOk, the request stayed in
   * flight for good, a wait on it never returned, and every deregistration
   * afterwards was refused as busy. */
  EngineConfig cfg = explicit_cfg();
  Fixture f;
  CHECK(f.setup(cfg, MockConfig{}));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 0, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 0, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kInvalidArgument);
  CHECK(req == nullptr);
  CHECK_EQ(f.provider->submitted_subops(), 0u);
  CHECK_EQ(f.engine->stats().requests_accepted, uint64_t(0));
  /* Nothing is in flight, so the region can go at once. */
  CHECK_STATUS(f.engine->deregister_memory(f.dst_region), Status::kOk);
}

/* ---- Submission queue bound ---- */

HUX_TEST(queue_full_returns_would_block) {
  EngineConfig cfg = explicit_cfg();
  cfg.max_inflight_requests = 2;
  MockConfig mock;
  mock.move_data = false;
  Fixture f;
  CHECK(f.setup(cfg, mock));

  std::vector<RequestPtr> reqs;
  Status last = Status::kOk;
  for (int i = 0; i < 5; ++i) {
    RegionView local, remote;
    CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
    CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
    RequestPtr r;
    last = f.engine->read(f.peer.get(), local, remote, {}, &r);
    if (last != Status::kOk) break;
    reqs.push_back(r);
  }
  /* A full queue must yield kWouldBlock: not accepted, no side effect. That
   * is a different thing from a failed transfer. */
  CHECK_STATUS(last, Status::kWouldBlock);
}

/* ---- Cancel and timeout ---- */

HUX_TEST(timeout_does_not_cancel_request) {
  EngineConfig cfg = explicit_cfg();
  MockConfig mock;
  mock.move_data = false;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);

  /* Without progress the request stays in flight. */
  CHECK_STATUS(req->wait(10), Status::kTimeout);
  /* After the timeout the request is still alive and not terminal. */
  bool done = true;
  CHECK_STATUS(req->test(&done), Status::kOk);
  CHECK_EQ(done, false);
  CHECK(req->state() != RequestState::kCancelled);
  /* Repeated waits agree. */
  CHECK_STATUS(req->wait(10), Status::kTimeout);
}

HUX_TEST(cancel_reaches_cancelled_safe) {
  EngineConfig cfg = explicit_cfg();
  MockConfig mock;
  mock.move_data = false;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(req->cancel(), Status::kOk);
  CHECK(req->state() == RequestState::kDraining);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 100; ++i) {
    f.engine->poll_completions(16, &done);
    if (req->state() == RequestState::kCancelled) break;
  }
  CHECK(req->state() == RequestState::kCancelled);
  /* A cancelled request must never publish a success ready. */
  CHECK(req->reached(Stage::kCancelledSafe));
  CHECK(!req->reached(Stage::kTargetReady));
}

HUX_TEST(cancel_after_success_reports_too_late) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  /* The handoff is irrevocable, so report cancel-too-late rather than
   * pretending it worked. */
  CHECK_STATUS(req->cancel(), Status::kInvalidArgument);
}

/* ---- Transfer errors ---- */

HUX_TEST(subop_error_marks_may_have_modified_target) {
  EngineConfig cfg = explicit_cfg();
  MockConfig mock;
  mock.fail_subops = true;
  mock.move_data = false;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  /* Into the region exported for writing. */
  std::vector<uint8_t> desc;
  CHECK_STATUS(f.dst_region->export_descriptor(&desc), Status::kOk);
  RemoteRegionPtr target;
  CHECK_STATUS(f.peer->import_region(desc, &target), Status::kOk);
  RegionView local, remote;
  CHECK_STATUS(f.src_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(target->view(0, 256, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->write(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  f.drain(req, 500);
  CHECK(!req->error().ok());
  /* Batches are not atomic, so a failure may have changed part of the
   * target; that bit has to reach the caller. */
  CHECK_EQ(req->error().may_have_modified_target, true);
  CHECK(req->reached(Stage::kFailedSafe));
}

/* ---- Region lifetime ---- */

HUX_TEST(retired_region_rejects_new_submits) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  /* Deregistration blocks new submissions first. */
  std::static_pointer_cast<MemoryRegionImpl>(f.dst_region)->retire();
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kStaleGeneration);
}

HUX_TEST(invalidated_remote_region_rejects_view) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  std::static_pointer_cast<RemoteRegionImpl>(f.remote_src)->invalidate();
  RegionView v;
  CHECK_STATUS(f.remote_src->view(0, 256, &v), Status::kStaleGeneration);
}

/* ---- rkey vs lkey ---- */

HUX_TEST(exported_descriptor_uses_remote_key_not_local) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  auto impl = std::static_pointer_cast<MemoryRegionImpl>(f.src_region);
  std::vector<uint8_t> desc;
  CHECK_STATUS(impl->export_descriptor(&desc), Status::kOk);
  RegionDescriptor d;
  CHECK_STATUS(decode_descriptor(desc, &d), Status::kOk);
  /* The mock keeps rkey != lkey, so exporting the lkey by mistake fails here
   * rather than on the one device where the two coincide. */
  CHECK_EQ(d.remote_key, impl->remote_key());
  CHECK(d.remote_key != impl->local_key());
}

/* ---- Both progress modes ---- */

HUX_TEST(thread_progress_mode_completes_without_explicit_poll) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread;
  Fixture f;
  CHECK(f.setup(cfg));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  /* Thread mode must not depend on the caller happening to call wait(). */
  CHECK_STATUS(req->wait(3000), Status::kOk);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

/* ---- Concurrent submission ---- */

HUX_TEST(concurrent_submits_are_accounted_correctly) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread;
  cfg.chunk_bytes = 256;
  Fixture f;
  CHECK(f.setup(cfg, {}, 8192));

  constexpr int kThreads = 4;
  constexpr int kPerThread = 8;
  std::vector<std::thread> ts;
  std::atomic<int> ok_count{0};
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        RegionView local, remote;
        uint64_t off = static_cast<uint64_t>((t * kPerThread + i) * 256);
        if (f.dst_region->view(off, 256, &local) != Status::kOk) return;
        if (f.remote_src->view(off, 256, &remote) != Status::kOk) return;
        RequestPtr r;
        if (f.engine->read(f.peer.get(), local, remote, {}, &r) != Status::kOk)
          continue;
        if (r->wait(5000) == Status::kOk) ok_count.fetch_add(1);
      }
    });
  }
  for (auto& th : ts) th.join();
  CHECK_EQ(ok_count.load(), kThreads * kPerThread);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), kThreads * kPerThread * 256),
           0);
}

/* ---- Configuration validation ---- */

HUX_TEST(config_rejects_conflicting_parameters) {
  std::string why;
  EngineConfig c;
  c.qp_per_peer = 0;
  CHECK_STATUS(c.validate(&why), Status::kInvalidArgument);
  CHECK(!why.empty());

  EngineConfig c2;
  c2.cc = CongestionControl::kFixedWindow;
  c2.chunk_bytes = 1 << 20;
  c2.cc_window_bytes = 1024; /* Smaller than a chunk: every request stalls. */
  CHECK_STATUS(c2.validate(&why), Status::kInvalidArgument);

  EngineConfig ok_cfg;
  CHECK_STATUS(ok_cfg.validate(&why), Status::kOk);
  CHECK(why.empty());
}

/* ---- close ---- */

HUX_TEST(close_drains_then_succeeds) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 1024, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 1024, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.engine->close(2000), Status::kOk);
  /* No new work is accepted after close. */
  RequestPtr req2;
  CHECK(f.engine->read(f.peer.get(), local, remote, {}, &req2) != Status::kOk);
}
