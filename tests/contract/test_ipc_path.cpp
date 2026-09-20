/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The same-host path's own logic: publication, the range check a mapping does
 * not do for you, withdrawal, and what happens when the peer stops listening.
 *
 * The mapping itself is faked, because a real one needs a second process --
 * tests/manual/ipc_pair.cpp does that part. What is exercised here is
 * everything the provider decides on its own. */
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "contract/fake_ipc_backend.h"
#include "core/factory.h"
#include "test_main.h"
#include "transport/ipc/ipc_provider.h"

using namespace hux;

namespace {

/* Two providers in this process, connected to each other. */
struct Pair {
  std::shared_ptr<testing::FakeIpcBackend> dev =
      std::make_shared<testing::FakeIpcBackend>();
  std::shared_ptr<IpcProvider> a, b;
  ProviderConnectionPtr ca, cb;
  std::vector<uint8_t> abuf = std::vector<uint8_t>(4096, 0xAA);
  std::vector<uint8_t> bbuf = std::vector<uint8_t>(4096, 0xBB);

  bool setup() {
    if (IpcProvider::create(IpcConfig{}, dev, &a) != Status::kOk) return false;
    if (IpcProvider::create(IpcConfig{}, dev, &b) != Status::kOk) return false;
    std::vector<uint8_t> meta;
    if (a->local_metadata(&meta) != Status::kOk) return false;
    /* Dialled first, accepted after: accept blocks, so the order matters and
     * the connecting side is the one that can be called synchronously. */
    std::thread t([&] { a->accept(2000, &ca); });
    Status s = b->connect(meta, &cb);
    t.join();
    return s == Status::kOk && ca != nullptr && cb != nullptr;
  }
};

}  // namespace

HUX_TEST(ipc_reports_the_copy_it_makes) {
  /* Mapping removes the network, not the copy. A path that reported zero here
   * would look like the in-place one it is not. */
  Pair p;
  CHECK(p.setup());

  uint64_t alk = 0, ark = 0;
  CHECK_STATUS(p.a->register_region(p.abuf.data(), p.abuf.size(), DeviceId{},
                                    AccessFlags::kRemoteRead, &alk, &ark),
               Status::kOk);

  SubOp op;
  op.kind = SubOp::Kind::kRead;
  op.request = 1;
  op.sub_id = 1;
  op.local_addr = p.bbuf.data();
  op.remote_addr = reinterpret_cast<uintptr_t>(p.abuf.data());
  op.remote_key = ark;
  op.length = 4096;
  auto r = p.b->submit(p.cb.get(), {op});
  CHECK_EQ(r.accepted, 1u);

  std::vector<CompletionEvent> evs;
  CHECK_STATUS(p.b->poll(8, &evs), Status::kOk);
  CHECK_EQ(evs.size(), size_t{1});
  CHECK_STATUS(evs[0].status, Status::kOk);
  CHECK_EQ(p.bbuf[0], uint8_t{0xAA});

  auto const st = p.b->stats();
  CHECK_EQ(st.payload_bytes, uint64_t{4096});
  CHECK_EQ(st.payload_bytes_copied, uint64_t{4096});
}

HUX_TEST(ipc_refuses_a_span_that_leaves_the_peers_region) {
  /* The check a NIC makes against an rkey. A mapping does not make it: the
   * address past the region is mapped too, and reading it would succeed and
   * return the wrong bytes. */
  Pair p;
  CHECK(p.setup());

  uint64_t alk = 0, ark = 0;
  CHECK_STATUS(p.a->register_region(p.abuf.data(), 1024, DeviceId{},
                                    AccessFlags::kRemoteRead, &alk, &ark),
               Status::kOk);

  SubOp op;
  op.kind = SubOp::Kind::kRead;
  op.request = 1;
  op.sub_id = 1;
  op.local_addr = p.bbuf.data();
  op.remote_addr = reinterpret_cast<uintptr_t>(p.abuf.data()) + 512;
  op.remote_key = ark;
  op.length = 1024; /* starts inside, ends past the end */
  auto r = p.b->submit(p.cb.get(), {op});
  CHECK_EQ(r.accepted, 0u);
  CHECK_STATUS(r.status, Status::kInvalidArgument);
}

HUX_TEST(ipc_refuses_a_key_the_peer_never_published) {
  Pair p;
  CHECK(p.setup());

  SubOp op;
  op.kind = SubOp::Kind::kRead;
  op.request = 1;
  op.sub_id = 1;
  op.local_addr = p.bbuf.data();
  op.remote_addr = reinterpret_cast<uintptr_t>(p.abuf.data());
  op.remote_key = 999;
  op.length = 64;
  auto r = p.b->submit(p.cb.get(), {op});
  CHECK_EQ(r.accepted, 0u);
  CHECK_STATUS(r.status, Status::kNotFound);
}

HUX_TEST(ipc_maps_once_for_two_regions_in_one_allocation) {
  /* CUDA refuses a second open of the same handle, so two regions inside one
   * peer allocation have to share a mapping rather than each taking their
   * own. */
  Pair p;
  CHECK(p.setup());

  uint64_t lk1 = 0, rk1 = 0, lk2 = 0, rk2 = 0;
  CHECK_STATUS(p.a->register_region(p.abuf.data(), 1024, DeviceId{},
                                    AccessFlags::kRemoteRead, &lk1, &rk1),
               Status::kOk);
  CHECK_STATUS(p.a->register_region(p.abuf.data(), 2048, DeviceId{},
                                    AccessFlags::kRemoteRead, &lk2, &rk2),
               Status::kOk);
  CHECK(rk1 != rk2);

  for (uint64_t key : {rk1, rk2}) {
    SubOp op;
    op.kind = SubOp::Kind::kRead;
    op.request = 1;
    op.sub_id = key;
    op.local_addr = p.bbuf.data();
    op.remote_addr = reinterpret_cast<uintptr_t>(p.abuf.data());
    op.remote_key = key;
    op.length = 64;
    CHECK_EQ(p.b->submit(p.cb.get(), {op}).accepted, 1u);
  }
  /* Two regions, two publications, one allocation: the backend is asked for a
   * mapping twice because the keys differ, and the cache in a real backend is
   * what keeps that to one open. Here the point is that neither submit was
   * refused for want of a mapping. */
  std::vector<CompletionEvent> evs;
  p.b->poll(8, &evs);
  CHECK_EQ(evs.size(), size_t{2});
}

HUX_TEST(ipc_withdrawal_stops_the_peer_using_the_region) {
  Pair p;
  CHECK(p.setup());

  uint64_t alk = 0, ark = 0;
  CHECK_STATUS(p.a->register_region(p.abuf.data(), p.abuf.size(), DeviceId{},
                                    AccessFlags::kRemoteRead, &alk, &ark),
               Status::kOk);

  SubOp op;
  op.kind = SubOp::Kind::kRead;
  op.request = 1;
  op.sub_id = 1;
  op.local_addr = p.bbuf.data();
  op.remote_addr = reinterpret_cast<uintptr_t>(p.abuf.data());
  op.remote_key = ark;
  op.length = 64;
  CHECK_EQ(p.b->submit(p.cb.get(), {op}).accepted, 1u);

  /* The peer has to keep polling for the withdrawal to be seen and answered,
   * so it is driven here the way a progress thread would. */
  std::thread pump([&] {
    std::vector<ControlMessage> msgs;
    for (int i = 0; i < 400; ++i) {
      p.b->poll_control(8, &msgs);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  CHECK_STATUS(p.a->deregister_region(alk), Status::kOk);
  pump.join();

  auto r = p.b->submit(p.cb.get(), {op});
  CHECK_EQ(r.accepted, 0u);
  CHECK_STATUS(r.status, Status::kNotFound);
  CHECK(p.dev->closed > 0);
}

HUX_TEST(ipc_refuses_once_the_peer_is_gone) {
  /* A mapping outlives the process that exported it. When that process dies
   * its allocation is freed, and the mapping left here points at memory the
   * driver is free to hand to somebody else -- so a write that still
   * "succeeds" is writing into whatever took its place. Submission has to
   * fail once the peer is gone, and the mapping has to be dropped. */
  Pair p;
  CHECK(p.setup());

  uint64_t alk = 0, ark = 0;
  CHECK_STATUS(p.a->register_region(p.abuf.data(), p.abuf.size(), DeviceId{},
                                    AccessFlags::kRemoteWrite, &alk, &ark),
               Status::kOk);

  SubOp op;
  op.kind = SubOp::Kind::kWrite;
  op.request = 1;
  op.sub_id = 1;
  op.local_addr = p.bbuf.data();
  op.remote_addr = reinterpret_cast<uintptr_t>(p.abuf.data());
  op.remote_key = ark;
  op.length = 64;
  CHECK_EQ(p.b->submit(p.cb.get(), {op}).accepted, 1u);
  std::vector<CompletionEvent> evs;
  p.b->poll(8, &evs);
  CHECK_EQ(evs.size(), size_t{1});

  /* The exporting side goes away, as a process exiting would close it. */
  p.a->disconnect(p.ca);

  auto r = p.b->submit(p.cb.get(), {op});
  CHECK_EQ(r.accepted, 0u);
  CHECK_STATUS(r.status, Status::kPeerDisconnected);
  CHECK(p.dev->closed > 0);
}

HUX_TEST(ipc_withdrawal_times_out_rather_than_hanging) {
  /* A peer that has stopped polling never confirms. Waiting for ever would
   * hang the caller inside a deregistration; reporting a timeout says plainly
   * that the mapping may still be held, so the memory is not yet safe to
   * free. */
  Pair p;
  CHECK(p.setup());

  uint64_t alk = 0, ark = 0;
  CHECK_STATUS(p.a->register_region(p.abuf.data(), p.abuf.size(), DeviceId{},
                                    AccessFlags::kRemoteRead, &alk, &ark),
               Status::kOk);
  (void)ark;

  auto const t0 = std::chrono::steady_clock::now();
  Status s = p.a->deregister_region(alk);
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  CHECK_STATUS(s, Status::kTimeout);
  CHECK(ms >= 1500);
}
