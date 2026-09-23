/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include <thread>
#include <vector>

#include "contract/fake_ipc_backend.h"
#include "core/factory.h"
#include "core/region_impl.h"
#include "test_main.h"
#include "transport/ipc/ipc_provider.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

/* An engine holding both transports, in the order a caller would give them:
 * the closer one first, the network one as what everything else falls to. */
struct TwoPathEngine {
  std::shared_ptr<DeviceBackend> dev;
  std::shared_ptr<IpcProvider> ipc;
  std::shared_ptr<MockProvider> net;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> buf = std::vector<uint8_t>(8192, 0x5A);

  bool setup(std::shared_ptr<DeviceBackend> backend) {
    dev = std::move(backend);
    if (IpcProvider::create(IpcConfig{}, dev, &ipc) != Status::kOk)
      return false;
    net = std::make_shared<MockProvider>(MockConfig{});
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    std::vector<TransportProviderPtr> providers{ipc, net};
    return make_engine(cfg, dev, providers, &engine) == Status::kOk;
  }
};

}  // namespace

HUX_TEST(a_region_carries_a_key_for_every_transport_that_took_it) {
  /* Which path a peer will arrive on is not known when memory is registered,
   * so every transport that can hold it gets a key and all of them travel. */
  TwoPathEngine e;
  CHECK(e.setup(std::make_shared<testing::FakeIpcBackend>()));

  MemoryRegionPtr r;
  CHECK_STATUS(e.engine->register_memory(e.buf.data(), e.buf.size(),
                                         AccessFlags::kRemoteRead, &r),
               Status::kOk);
  std::vector<uint8_t> desc;
  CHECK_STATUS(r->export_descriptor(&desc), Status::kOk);

  RegionDescriptor d;
  CHECK_STATUS(decode_descriptor(desc, &d), Status::kOk);
  CHECK_EQ(d.provider_keys.size(), size_t{2});
  CHECK(d.provider_keys[0].provider == "ipc");
  CHECK(d.provider_keys[1].provider == "mock");
}

HUX_TEST(a_transport_that_refused_the_memory_contributes_no_key) {
  /* IPC cannot export host memory. The registration still succeeds, on the
   * transport that could take it, and the descriptor says so rather than
   * offering a key that would fail on use. */
  TwoPathEngine e;
  CHECK(e.setup(std::make_shared<testing::NoIpcBackend>()));

  MemoryRegionPtr r;
  CHECK_STATUS(e.engine->register_memory(e.buf.data(), e.buf.size(),
                                         AccessFlags::kRemoteRead, &r),
               Status::kOk);
  std::vector<uint8_t> desc;
  CHECK_STATUS(r->export_descriptor(&desc), Status::kOk);

  RegionDescriptor d;
  CHECK_STATUS(decode_descriptor(desc, &d), Status::kOk);
  CHECK_EQ(d.provider_keys.size(), size_t{1});
  CHECK(d.provider_keys[0].provider == "mock");
}

HUX_TEST(a_peer_on_this_host_is_reached_over_the_closer_transport) {
  TwoPathEngine a, b;
  CHECK(a.setup(std::make_shared<testing::FakeIpcBackend>()));
  CHECK(b.setup(std::make_shared<testing::FakeIpcBackend>()));

  std::vector<uint8_t> meta;
  CHECK_STATUS(b.engine->local_metadata(&meta), Status::kOk);

  /* The passive side has to answer; an engine has no accept of its own, the
   * same as on the network path. */
  ProviderConnectionPtr accepted;
  std::thread t([&] { b.ipc->accept(2000, &accepted); });
  PeerPtr peer;
  Status s = a.engine->add_peer(meta, &peer);
  t.join();

  CHECK_STATUS(s, Status::kOk);
  CHECK(peer->caps().provider == "ipc");
  CHECK(peer->caps().path == PathKind::kIpc);
  CHECK(peer->caps().place == PeerPlace::kSameProcess);
}

HUX_TEST(a_peer_on_another_host_is_reached_over_the_network) {
  /* Same engine, same two transports. Only the peer's identity differs, and
   * that is what decides: IPC handles name nothing on another machine. */
  TwoPathEngine a, b;
  CHECK(a.setup(std::make_shared<testing::FakeIpcBackend>()));
  CHECK(b.setup(std::make_shared<testing::FakeIpcBackend>()));

  std::vector<uint8_t> meta;
  CHECK_STATUS(b.engine->local_metadata(&meta), Status::kOk);
  /* Rewrite the host, which is what a peer elsewhere would have sent. */
  Identity foreign;
  foreign.host = 0xFFFFFFFFull;
  foreign.process = 7;
  foreign.engine = 1;
  std::vector<uint8_t> head;
  encode_identity(foreign, &head);
  std::vector<uint8_t> rewritten = head;
  rewritten.insert(rewritten.end(), meta.begin() + kIdentityBytes, meta.end());

  PeerPtr peer;
  CHECK_STATUS(a.engine->add_peer(rewritten, &peer), Status::kOk);
  CHECK(peer->caps().provider == "mock");
  CHECK(peer->caps().path == PathKind::kRdma);
  CHECK(peer->caps().place == PeerPlace::kAnotherHost);
}

HUX_TEST(a_peer_reached_over_ucx_says_so) {
  /* Every transport that was neither local nor IPC was reported as the
   * native RDMA path, UCX included. */
  MockConfig mc;
  mc.name = "ucx";
  auto prov = std::make_shared<MockProvider>(mc);
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> e;
  CHECK_STATUS(make_engine(cfg, nullptr, prov, &e), Status::kOk);
  std::vector<uint8_t> meta;
  CHECK_STATUS(e->local_metadata(&meta), Status::kOk);
  PeerPtr peer;
  CHECK_STATUS(e->add_peer(meta, &peer), Status::kOk);
  CHECK(peer->caps().provider == "ucx");
  CHECK(peer->caps().path == PathKind::kUcx);
}

HUX_TEST(a_region_exported_for_another_path_is_refused_not_misused) {
  /* The peer exported a region its network transport registered, and this
   * side reached it over IPC. There is no key here that means anything, and
   * saying so is the whole point: the alternative is an rkey handed to a
   * mapping, accepted locally and refused somewhere far away. */
  TwoPathEngine a, b;
  CHECK(a.setup(std::make_shared<testing::FakeIpcBackend>()));
  /* b cannot export to another process, so its descriptor carries only the
   * network key. */
  CHECK(b.setup(std::make_shared<testing::NoIpcBackend>()));

  MemoryRegionPtr r;
  CHECK_STATUS(b.engine->register_memory(b.buf.data(), b.buf.size(),
                                         AccessFlags::kRemoteRead, &r),
               Status::kOk);
  std::vector<uint8_t> desc;
  CHECK_STATUS(r->export_descriptor(&desc), Status::kOk);

  std::vector<uint8_t> meta;
  CHECK_STATUS(b.engine->local_metadata(&meta), Status::kOk);
  ProviderConnectionPtr accepted;
  std::thread t([&] { b.ipc->accept(2000, &accepted); });
  PeerPtr peer;
  Status s = a.engine->add_peer(meta, &peer);
  t.join();
  CHECK_STATUS(s, Status::kOk);
  CHECK(peer->caps().provider == "ipc");

  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kUnsupported);
}
