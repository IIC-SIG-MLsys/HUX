/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * progress() runs on one thread at a time.
 *
 * A regression. Nothing stopped two at once: close() drove progress itself
 * while the progress thread was still running, and in explicit mode every
 * poll call drives it from whichever thread makes the call. A provider's
 * control reader keeps a partial message between calls and is not made for
 * two callers, so two at once interleaved one stream's bytes; the next header
 * then failed to decode and every handoff and acknowledgement after it was
 * lost. The mock counts callers that overlap in its control reader, and
 * holds each call long enough that two unserialized ones always do. */
#include <thread>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

struct OnePeer {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src, dst;
  MemoryRegionPtr sreg, dreg;
  PeerPtr peer;
  RemoteRegionPtr remote;

  bool setup(EngineConfig cfg, MockConfig mc) {
    provider = std::make_shared<MockProvider>(mc);
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

  Status write(RequestPtr* out) {
    RegionView lv, rv;
    if (sreg->view(0, 4096, &lv) != Status::kOk) return Status::kInternal;
    if (remote->view(0, 4096, &rv) != Status::kOk) return Status::kInternal;
    return engine->write(peer.get(), lv, rv, {}, out);
  }
};

}  // namespace

HUX_TEST(close_does_not_drive_progress_beside_the_progress_thread) {
  MockConfig mc;
  mc.never_complete = true; /* so close() has something to wait on */
  mc.control_poll_us = 200;
  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread;
  OnePeer f;
  CHECK(f.setup(cfg, mc));
  RequestPtr req;
  CHECK_STATUS(f.write(&req), Status::kOk);

  /* It waits the whole time, driving progress the while. */
  CHECK_STATUS(f.engine->close(300), Status::kTimeout);
  CHECK_EQ(f.provider->overlapping_control_polls(), uint64_t(0));
}

HUX_TEST(pollers_on_two_threads_take_turns) {
  MockConfig mc;
  mc.control_poll_us = 50;
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  OnePeer f;
  CHECK(f.setup(cfg, mc));

  auto poller = [&](bool completions) {
    for (int i = 0; i < 300; ++i) {
      if (completions) {
        std::vector<RequestPtr> done;
        f.engine->poll_completions(16, &done);
      } else {
        std::vector<Notification> notes;
        f.engine->poll_notifications(16, &notes);
      }
    }
  };
  std::thread a(poller, true), b(poller, false);
  a.join();
  b.join();
  CHECK_EQ(f.provider->overlapping_control_polls(), uint64_t(0));
}
