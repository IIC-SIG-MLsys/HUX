/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The same-process path. Both ends can reach the memory directly, which is
 * exactly why it needs testing: a local path that succeeds where a remote one
 * refuses teaches callers habits that break when the peer moves to another
 * host. */
#include <cstring>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/local/local_provider.h"

using namespace hux;

namespace {

struct Pair {
  std::shared_ptr<LocalProvider> pa, pb;
  std::unique_ptr<Engine> a, b;
  std::vector<uint8_t> buf_a, buf_b;
  MemoryRegionPtr reg_a, reg_b;
  PeerPtr peer_a;
  RemoteRegionPtr remote_b;

  bool setup(size_t bytes = 8192, uint32_t cache_entries = 64) {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    cfg.registration_cache_entries = cache_entries;
    if (LocalProvider::create(&pa) != Status::kOk) return false;
    if (LocalProvider::create(&pb) != Status::kOk) return false;
    pa->pair_with(pb);
    pb->pair_with(pa);
    if (make_engine(cfg, nullptr, pa, &a) != Status::kOk) return false;
    if (make_engine(cfg, nullptr, pb, &b) != Status::kOk) return false;

    buf_a.assign(bytes, 0);
    buf_b.assign(bytes, 0);
    for (size_t i = 0; i < bytes; ++i)
      buf_b[i] = static_cast<uint8_t>(i * 13 + 5);

    if (a->register_memory(buf_a.data(), bytes, AccessFlags::kLocalWrite,
                           &reg_a) != Status::kOk)
      return false;
    if (b->register_memory(buf_b.data(), bytes, AccessFlags::kRemoteRead,
                           &reg_b) != Status::kOk)
      return false;

    std::vector<uint8_t> meta, desc;
    b->local_metadata(&meta);
    if (a->add_peer(meta, &peer_a) != Status::kOk) return false;
    reg_b->export_descriptor(&desc);
    return peer_a->import_region(desc, &remote_b) == Status::kOk;
  }

  Status drive(RequestPtr const& r) {
    std::vector<RequestPtr> done;
    for (int i = 0; i < 200; ++i) {
      a->poll_completions(16, &done);
      bool fin = false;
      r->test(&fin);
      if (fin) return r->wait(1000);
    }
    return Status::kTimeout;
  }
};

}  // namespace

HUX_TEST(two_engines_in_one_process_transfer) {
  Pair p;
  CHECK(p.setup());

  RegionView lv, rv;
  CHECK_STATUS(p.reg_a->view(0, 8192, &lv), Status::kOk);
  CHECK_STATUS(p.remote_b->view(0, 8192, &rv), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(p.a->read(p.peer_a.get(), lv, rv, {}, &req), Status::kOk);
  CHECK_STATUS(p.drive(req), Status::kOk);
  CHECK_EQ(std::memcmp(p.buf_a.data(), p.buf_b.data(), 8192), 0);
}

HUX_TEST(the_local_path_reports_its_copies) {
  /* It really does copy. Claiming zero would make this path look like the
   * in-place one it is not, and a comparison against RDMA would be wrong by
   * exactly the amount that matters. */
  Pair p;
  CHECK(p.setup());
  RegionView lv, rv;
  CHECK_STATUS(p.reg_a->view(0, 4096, &lv), Status::kOk);
  CHECK_STATUS(p.remote_b->view(0, 4096, &rv), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(p.a->read(p.peer_a.get(), lv, rv, {}, &req), Status::kOk);
  CHECK_STATUS(p.drive(req), Status::kOk);

  EngineStats st = p.a->stats();
  CHECK_EQ(st.payload_bytes, 4096u);
  CHECK_EQ(st.payload_bytes_copied, 4096u);
}

HUX_TEST(an_unknown_remote_key_is_refused_not_dereferenced) {
  /* The dangerous one. A peer's address is a key plus an offset, resolved
   * through the registry. Taking it as a pointer would work here and be a
   * serious bug the moment the peer is remote. */
  void* p = LocalRegistry::instance().resolve(0xdeadbeef, 0, 4096);
  CHECK(p == nullptr);
}

HUX_TEST(an_out_of_range_access_is_refused) {
  std::vector<uint8_t> buf(4096, 0);
  uint64_t const key = LocalRegistry::instance().publish(buf.data(), 4096);
  auto const base = reinterpret_cast<uint64_t>(buf.data());

  CHECK(LocalRegistry::instance().resolve(key, base, 4096) == buf.data());
  CHECK(LocalRegistry::instance().resolve(key, base + 4096, 1) == nullptr);
  CHECK(LocalRegistry::instance().resolve(key, base, 4097) == nullptr);
  /* Below the region, and a wrapping range: neither may appear to fit. */
  CHECK(LocalRegistry::instance().resolve(key, base - 16, 32) == nullptr);
  CHECK(LocalRegistry::instance().resolve(key, ~0ull - 8, 16) == nullptr);

  LocalRegistry::instance().withdraw(key);
  CHECK(LocalRegistry::instance().resolve(key, base, 4096) == nullptr);
}

HUX_TEST(a_withdrawn_registration_stops_serving_transfers) {
  /* Without the registration cache, so deregistering really does withdraw
   * the key. With it enabled the registration is kept for reuse and the peer
   * can still reach it until eviction -- worth knowing, and covered
   * separately below. */
  Pair p;
  CHECK(p.setup(8192, 0));
  RegionView lv, rv;
  CHECK_STATUS(p.reg_a->view(0, 4096, &lv), Status::kOk);
  CHECK_STATUS(p.remote_b->view(0, 4096, &rv), Status::kOk);

  /* Take the peer's registration away without telling this side, which is
   * what a crashed peer looks like. */
  CHECK_STATUS(p.b->deregister_memory(p.reg_b), Status::kOk);
  p.reg_b.reset();

  RequestPtr req;
  Status s = p.a->read(p.peer_a.get(), lv, rv, {}, &req);
  if (s == Status::kOk) {
    p.drive(req);
    CHECK(is_terminal(req->state()));
    CHECK(req->state() != RequestState::kSucceeded);
  } else {
    CHECK(s != Status::kOk);
  }
}

HUX_TEST(the_cache_keeps_a_peer_reachable_after_deregistration) {
  /* Stated rather than discovered later: while a registration is held for
   * reuse, a peer that already imported it can still reach the memory. The
   * roadmap says the same thing about hardware -- a generation stops software
   * from using a stale descriptor but not a NIC holding the old key -- so
   * reusing the memory itself has to wait for the registration to go, not
   * merely for the handle to. */
  Pair p;
  CHECK(p.setup(8192, 64));
  RegionView lv, rv;
  CHECK_STATUS(p.reg_a->view(0, 4096, &lv), Status::kOk);
  CHECK_STATUS(p.remote_b->view(0, 4096, &rv), Status::kOk);

  CHECK_STATUS(p.b->deregister_memory(p.reg_b), Status::kOk);
  p.reg_b.reset();

  RequestPtr req;
  CHECK_STATUS(p.a->read(p.peer_a.get(), lv, rv, {}, &req), Status::kOk);
  CHECK_STATUS(p.drive(req), Status::kOk);
  CHECK_EQ(std::memcmp(p.buf_a.data(), p.buf_b.data(), 4096), 0);
}
