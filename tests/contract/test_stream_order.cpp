/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Ordering a caller's stream behind a request.
 *
 * A regression. wait_on() and wait_source_reusable_on() returned kOk whatever
 * the request's state. Nothing on the device can wait for a transfer still on
 * the network -- that needs a stream-side wait, which is not implemented --
 * so kOk for a request in flight let the caller launch a kernel that read the
 * target, or overwrote the source, while the adapter was still moving it.
 * They install something only once the stage is reached; before, kWouldBlock;
 * a request that failed says so. */
#include <vector>

#include "core/factory.h"
#include "fake_ipc_backend.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

class CountingBackend : public testing::FakeIpcBackend {
 public:
  Status make_visible(DeviceStream*, void*, uint64_t) override {
    ++visible;
    return Status::kOk;
  }
  int visible = 0;
};

class FakeStream : public DeviceStream {
 public:
  DeviceId device() const override { return DeviceId{}; }
  void* native_handle() const override { return nullptr; }
};

struct Setup {
  std::shared_ptr<CountingBackend> dev = std::make_shared<CountingBackend>();
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src, dst;
  MemoryRegionPtr sreg, dreg;
  PeerPtr peer;
  RemoteRegionPtr remote;
  FakeStream stream;

  bool make(MockConfig mc) {
    provider = std::make_shared<MockProvider>(mc);
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    if (make_engine(cfg, dev, provider, &engine) != Status::kOk) return false;
    src.assign(4096, 7);
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

  void settle(RequestPtr const& req) {
    std::vector<RequestPtr> done;
    for (int i = 0; i < 100 && !is_terminal(req->state()); ++i)
      engine->poll_completions(16, &done);
  }
};

}  // namespace

HUX_TEST(nothing_is_installed_behind_a_transfer_still_in_flight) {
  MockConfig mc;
  mc.never_complete = true;
  Setup s;
  CHECK(s.make(mc));
  RequestPtr req;
  CHECK_STATUS(s.write(&req), Status::kOk);
  s.settle(req);
  CHECK(!is_terminal(req->state()));

  CHECK_STATUS(req->wait_on(&s.stream), Status::kWouldBlock);
  CHECK_STATUS(req->wait_source_reusable_on(&s.stream), Status::kWouldBlock);
  CHECK_EQ(s.dev->visible, 0);
}

HUX_TEST(a_finished_transfer_orders_the_stream_behind_it) {
  Setup s;
  CHECK(s.make(MockConfig{}));
  RequestPtr req;
  CHECK_STATUS(s.write(&req), Status::kOk);
  s.settle(req);
  CHECK_STATUS(req->wait(1000), Status::kOk);

  CHECK_STATUS(req->wait_on(&s.stream), Status::kOk);
  CHECK_EQ(s.dev->visible, 1);
  CHECK_STATUS(req->wait_source_reusable_on(&s.stream), Status::kOk);
}

HUX_TEST(a_failed_transfer_says_so_instead_of_ordering_anything) {
  MockConfig mc;
  mc.fail_subops = true;
  mc.subop_error = Status::kTransportError;
  Setup s;
  CHECK(s.make(mc));
  RequestPtr req;
  CHECK_STATUS(s.write(&req), Status::kOk);
  s.settle(req);
  CHECK_STATUS(req->wait(1000), Status::kTransportError);

  CHECK_STATUS(req->wait_on(&s.stream), Status::kTransportError);
  CHECK_STATUS(req->wait_source_reusable_on(&s.stream),
               Status::kTransportError);
  CHECK_EQ(s.dev->visible, 0);
}
