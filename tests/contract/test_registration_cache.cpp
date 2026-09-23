/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Registration reuse. Registering memory is expensive, so a pool registered
 * once and transferred by views is the shape that performs -- but reuse has
 * to be exact. A region that only partly overlaps an existing one covers
 * bytes the hardware was never told about, and the transfer that follows
 * fails somewhere far from here. */
#include <vector>

#include "core/factory.h"
#include "core/region_impl.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

struct CacheFixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> pool;

  bool setup(size_t bytes = 1 << 20) {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    provider = std::make_shared<MockProvider>(MockConfig{});
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
    pool.assign(bytes, 0);
    return true;
  }

  uint8_t* at(size_t off) { return pool.data() + off; }
};

}  // namespace

HUX_TEST(registering_the_same_range_twice_reuses_one_registration) {
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr a, b;
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 4096, AccessFlags::kRemoteRead, &a),
      Status::kOk);
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 4096, AccessFlags::kRemoteRead, &b),
      Status::kOk);

  /* Two independent handles over one registration. */
  CHECK(a->id() != b->id());
  CHECK_EQ(f.provider->registration_count(), 1u);
}

HUX_TEST(a_contained_subrange_reuses_the_enclosing_registration) {
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr whole, part;
  CHECK_STATUS(f.engine->register_memory(f.at(0), 1 << 20,
                                         AccessFlags::kRemoteRead, &whole),
               Status::kOk);
  CHECK_STATUS(f.engine->register_memory(f.at(4096), 8192,
                                         AccessFlags::kRemoteRead, &part),
               Status::kOk);

  CHECK_EQ(f.provider->registration_count(), 1u);
  /* The handle still describes what was asked for, not the pool. */
  CHECK_EQ(part->length(), 8192u);
  CHECK(part->base() == f.at(4096));
}

HUX_TEST(a_partly_overlapping_range_is_registered_separately) {
  /* The dangerous case. Reusing here would leave the tail outside anything
   * the hardware knows about, and the failure would surface during a
   * transfer rather than at registration. */
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr first, second;
  CHECK_STATUS(f.engine->register_memory(f.at(0), 8192,
                                         AccessFlags::kRemoteRead, &first),
               Status::kOk);
  CHECK_STATUS(f.engine->register_memory(f.at(4096), 8192,
                                         AccessFlags::kRemoteRead, &second),
               Status::kOk);

  CHECK_EQ(f.provider->registration_count(), 2u);
}

HUX_TEST(wider_permissions_are_not_taken_from_a_narrower_registration) {
  /* A range registered read-only cannot serve a request that needs writing:
   * the hardware would refuse the write, far from the call that assumed it
   * was allowed. */
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr ro, rw;
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 8192, AccessFlags::kRemoteRead, &ro),
      Status::kOk);
  CHECK_STATUS(f.engine->register_memory(
                   f.at(0), 8192,
                   AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite, &rw),
               Status::kOk);
  CHECK_EQ(f.provider->registration_count(), 2u);
}

HUX_TEST(narrower_local_permissions_may_reuse_a_wider_registration) {
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr wide, narrow;
  CHECK_STATUS(f.engine->register_memory(f.at(0), 8192,
                                         AccessFlags::kLocalRead |
                                             AccessFlags::kLocalWrite |
                                             AccessFlags::kRemoteRead,
                                         &wide),
               Status::kOk);
  CHECK_STATUS(f.engine->register_memory(
                   f.at(0), 4096,
                   AccessFlags::kLocalRead | AccessFlags::kRemoteRead, &narrow),
               Status::kOk);
  CHECK_EQ(f.provider->registration_count(), 1u);
}

HUX_TEST(narrower_remote_permissions_get_a_registration_of_their_own) {
  /* A regression. Served from a registration a peer could write through, a
   * handle registered for reading only exported that registration's key --
   * and the key is what the adapter checks, so the peer could write. */
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr rw, ro;
  CHECK_STATUS(f.engine->register_memory(
                   f.at(0), 8192,
                   AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite, &rw),
               Status::kOk);
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 4096, AccessFlags::kRemoteRead, &ro),
      Status::kOk);
  CHECK_EQ(f.provider->registration_count(), 2u);
}

HUX_TEST(a_registration_outlives_handles_that_still_reference_it) {
  /* Releasing it while another handle still covers the same range would pull
   * it out from under that handle. Caching is disabled here so the only thing
   * keeping it alive is the reference. */
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  cfg.registration_cache_entries = 0;
  auto provider = std::make_shared<MockProvider>(MockConfig{});
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &engine), Status::kOk);
  std::vector<uint8_t> pool(1 << 20, 0);

  MemoryRegionPtr a, b;
  CHECK_STATUS(
      engine->register_memory(pool.data(), 8192, AccessFlags::kRemoteRead, &a),
      Status::kOk);
  /* Without a cache there is nothing to reuse, so this registers again. */
  CHECK_STATUS(
      engine->register_memory(pool.data(), 4096, AccessFlags::kRemoteRead, &b),
      Status::kOk);
  CHECK_EQ(provider->live_registrations(), 2u);

  CHECK_STATUS(engine->deregister_memory(a), Status::kOk);
  a.reset();
  CHECK_EQ(provider->live_registrations(), 1u);

  CHECK_STATUS(engine->deregister_memory(b), Status::kOk);
  b.reset();
  CHECK_EQ(provider->live_registrations(), 0u);
}

HUX_TEST(a_cached_registration_survives_deregistration) {
  /* Which is the point of the cache, and worth stating: with it enabled,
   * deregistering a handle does not release the hardware resource. It is
   * kept for the next caller and released on eviction. */
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr a;
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 8192, AccessFlags::kRemoteRead, &a),
      Status::kOk);
  CHECK_STATUS(f.engine->deregister_memory(a), Status::kOk);
  a.reset();
  CHECK_EQ(f.provider->live_registrations(), 1u);

  /* And the next caller gets it without registering again. */
  MemoryRegionPtr b;
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 4096, AccessFlags::kRemoteRead, &b),
      Status::kOk);
  CHECK_EQ(f.provider->registration_count(), 1u);
}

HUX_TEST(releasing_the_cache_lets_the_registration_go) {
  /* The counterpart to the test above: the cache holding a registration is
   * what makes a deregistration look like it did nothing, and this is how a
   * caller about to free the memory makes it happen. */
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr a;
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 8192, AccessFlags::kRemoteRead, &a),
      Status::kOk);
  CHECK_STATUS(f.engine->deregister_memory(a), Status::kOk);
  a.reset();
  CHECK_EQ(f.provider->live_registrations(), 1u);

  uint32_t released = 0;
  CHECK_STATUS(f.engine->release_cached_registrations(&released), Status::kOk);
  CHECK_EQ(released, 1u);
  CHECK_EQ(f.provider->live_registrations(), 0u);
}

HUX_TEST(releasing_the_cache_keeps_what_is_still_held) {
  /* A live handle is not a cache entry nobody wants. Releasing it here would
   * take the registration out from under a caller that is still using it,
   * which is worse than holding memory a little longer. */
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr a;
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 8192, AccessFlags::kRemoteRead, &a),
      Status::kOk);

  uint32_t released = 0;
  CHECK_STATUS(f.engine->release_cached_registrations(&released), Status::kOk);
  CHECK_EQ(released, 0u);
  CHECK_EQ(f.provider->live_registrations(), 1u);
}

HUX_TEST(a_reused_registration_exports_the_right_remote_address) {
  /* A view into a pool has to advertise its own address, not the pool's, or
   * the peer writes to the wrong place -- and the bytes land somewhere valid,
   * so nothing reports an error. */
  CacheFixture f;
  CHECK(f.setup());

  MemoryRegionPtr whole, part;
  CHECK_STATUS(f.engine->register_memory(f.at(0), 1 << 20,
                                         AccessFlags::kRemoteRead, &whole),
               Status::kOk);
  CHECK_STATUS(f.engine->register_memory(f.at(65536), 4096,
                                         AccessFlags::kRemoteRead, &part),
               Status::kOk);

  std::vector<uint8_t> desc;
  CHECK_STATUS(part->export_descriptor(&desc), Status::kOk);
  RegionDescriptor d;
  CHECK_STATUS(decode_descriptor(desc, &d), Status::kOk);
  CHECK_EQ(d.base, reinterpret_cast<uint64_t>(f.at(65536)));
  CHECK_EQ(d.length, 4096u);
  /* Same underlying registration, so the same remote key. */
  std::vector<uint8_t> wdesc;
  CHECK_STATUS(whole->export_descriptor(&wdesc), Status::kOk);
  RegionDescriptor wd;
  CHECK_STATUS(decode_descriptor(wdesc, &wd), Status::kOk);
  CHECK_EQ(d.remote_key, wd.remote_key);
}

HUX_TEST(the_cache_has_a_bound) {
  /* Unbounded, it would keep every range ever registered alive and the
   * process would be holding registrations it has no use for. */
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  cfg.registration_cache_entries = 4;
  auto provider = std::make_shared<MockProvider>(MockConfig{});
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &engine), Status::kOk);

  std::vector<uint8_t> pool(1 << 20, 0);
  std::vector<MemoryRegionPtr> held;
  for (int i = 0; i < 8; ++i) {
    MemoryRegionPtr r;
    CHECK_STATUS(engine->register_memory(pool.data() + i * 8192, 4096,
                                         AccessFlags::kRemoteRead, &r),
                 Status::kOk);
    /* Released immediately, so nothing but the cache holds them. */
    CHECK_STATUS(engine->deregister_memory(r), Status::kOk);
  }
  CHECK(provider->live_registrations() <= 4u);
}
