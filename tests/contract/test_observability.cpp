/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Counters have to be right. One that drifts is worse than none: it gets
 * trusted, and the conclusion drawn from it is wrong in a way nothing else
 * contradicts. Each case here compares a counter against something
 * independently known. */
#include <string>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

bool contains(std::string const& haystack, std::string const& needle) {
  return haystack.find(needle) != std::string::npos;
}

struct ObsFixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> pool;
  PeerPtr peer;

  bool setup(EngineConfig cfg = {}) {
    cfg.progress = ProgressMode::kExplicit;
    provider = std::make_shared<MockProvider>(MockConfig{});
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
    pool.assign(1 << 20, 0);
    std::vector<uint8_t> meta;
    engine->local_metadata(&meta);
    return engine->add_peer(meta, &peer) == Status::kOk;
  }

  uint8_t* at(size_t off) { return pool.data() + off; }
};

}  // namespace

HUX_TEST(config_dump_reports_what_is_in_effect) {
  /* Defaults change and controllers get substituted; a result is worth little
   * without knowing which settings produced it. */
  EngineConfig cfg;
  cfg.qp_per_peer = 8;
  cfg.chunk_bytes = 1 << 18;
  cfg.cc = CongestionControl::kFixedWindow;
  cfg.cc_window_bytes = 1 << 22;
  cfg.preferred_provider = "rdma";

  std::string const dump = describe_config(cfg);
  CHECK(contains(dump, "\"qp_per_peer\":8"));
  CHECK(contains(dump, "\"chunk_bytes\":262144"));
  CHECK(contains(dump, "\"cc\":\"fixed_window\""));
  CHECK(contains(dump, "\"cc_window_bytes\":4194304"));
  CHECK(contains(dump, "\"preferred_provider\":\"rdma\""));
  /* Machine readable, so a run can be compared against another without
   * anyone retyping its settings. */
  CHECK(dump.front() == '{');
  CHECK(dump.back() == '}');
}

HUX_TEST(registration_counters_separate_new_from_reused) {
  ObsFixture f;
  CHECK(f.setup());

  MemoryRegionPtr a, b, c;
  /* One new registration. */
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 1 << 20, AccessFlags::kRemoteRead, &a),
      Status::kOk);
  /* Two contained in it. */
  CHECK_STATUS(
      f.engine->register_memory(f.at(4096), 4096, AccessFlags::kRemoteRead, &b),
      Status::kOk);
  CHECK_STATUS(
      f.engine->register_memory(f.at(8192), 4096, AccessFlags::kRemoteRead, &c),
      Status::kOk);

  EngineStats st = f.engine->stats();
  CHECK_EQ(st.registrations_created, 1u);
  CHECK_EQ(st.registrations_reused, 2u);
  /* Agrees with what the provider actually did. */
  CHECK_EQ(f.provider->registration_count(), st.registrations_created);
  CHECK_EQ(st.registration_cache_size, 1u);
}

HUX_TEST(notification_counters_distinguish_delivered_from_dropped) {
  /* A dropped notification is not a delivered one, and the difference is the
   * whole basis for back-pressure being believable. */
  EngineConfig cfg;
  cfg.notify_queue_depth = 1;
  ObsFixture f;
  CHECK(f.setup(cfg));

  RequestPtr a, b;
  CHECK_STATUS(f.engine->notify(f.peer.get(), {1}, &a), Status::kOk);
  CHECK_STATUS(f.engine->notify(f.peer.get(), {2}, &b), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 50; ++i) f.engine->poll_completions(8, &done);

  EngineStats st = f.engine->stats();
  CHECK_EQ(st.notifications_sent, 2u);
  CHECK_EQ(st.notifications_received, 1u);
  CHECK_EQ(st.notifications_dropped, 1u);
  /* And the counters match the outcomes: only the queued one succeeded. */
  CHECK(a->state() == RequestState::kSucceeded);
  CHECK(b->state() == RequestState::kWaitNotifyAck);
}

HUX_TEST(peak_inflight_records_the_high_water_mark) {
  /* Sizing the submission bound needs what the run demanded. The value at the
   * end is zero on any run that finished, which says nothing. */
  ObsFixture f;
  CHECK(f.setup());

  MemoryRegionPtr src, dst;
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 65536, AccessFlags::kRemoteRead, &src),
      Status::kOk);
  CHECK_STATUS(f.engine->register_memory(f.at(65536), 65536,
                                         AccessFlags::kLocalWrite, &dst),
               Status::kOk);
  std::vector<uint8_t> desc;
  src->export_descriptor(&desc);
  RemoteRegionPtr remote;
  CHECK_STATUS(f.peer->import_region(desc, &remote), Status::kOk);

  RegionView lv, rv;
  CHECK_STATUS(dst->view(0, 4096, &lv), Status::kOk);
  CHECK_STATUS(remote->view(0, 4096, &rv), Status::kOk);

  std::vector<RequestPtr> held;
  for (int i = 0; i < 5; ++i) {
    RequestPtr r;
    CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, {}, &r), Status::kOk);
    held.push_back(r);
  }

  std::vector<RequestPtr> done;
  for (int i = 0; i < 100; ++i) f.engine->poll_completions(16, &done);

  EngineStats st = f.engine->stats();
  CHECK_EQ(st.requests_accepted, 5u);
  CHECK(st.peak_inflight_requests > 0u);
  /* Everything finished, so nothing is in flight now -- which is exactly why
   * the peak has to be recorded as it happens. */
  CHECK_EQ(st.requests_succeeded, 5u);
}

HUX_TEST(counters_balance_across_a_run) {
  /* Whatever was accepted has to end up somewhere. A gap means a request went
   * missing, and no single counter would show it. */
  ObsFixture f;
  CHECK(f.setup());

  MemoryRegionPtr src, dst;
  CHECK_STATUS(
      f.engine->register_memory(f.at(0), 65536, AccessFlags::kRemoteRead, &src),
      Status::kOk);
  CHECK_STATUS(f.engine->register_memory(f.at(65536), 65536,
                                         AccessFlags::kLocalWrite, &dst),
               Status::kOk);
  std::vector<uint8_t> desc;
  src->export_descriptor(&desc);
  RemoteRegionPtr remote;
  CHECK_STATUS(f.peer->import_region(desc, &remote), Status::kOk);

  RegionView lv, rv;
  CHECK_STATUS(dst->view(0, 8192, &lv), Status::kOk);
  CHECK_STATUS(remote->view(0, 8192, &rv), Status::kOk);

  for (int i = 0; i < 12; ++i) {
    RequestPtr r;
    CHECK_STATUS(f.engine->read(f.peer.get(), lv, rv, {}, &r), Status::kOk);
  }
  std::vector<RequestPtr> done;
  for (int i = 0; i < 300; ++i) f.engine->poll_completions(32, &done);

  EngineStats st = f.engine->stats();
  CHECK_EQ(st.requests_accepted,
           st.requests_succeeded + st.requests_failed + st.requests_cancelled);
  CHECK_EQ(st.subops_posted, st.subops_completed + st.subops_failed);
  /* And the engine agrees with the provider about the bytes. */
  CHECK_EQ(st.payload_bytes, 12u * 8192u);
}

HUX_TEST(the_report_covers_both_halves_of_the_configuration) {
  /* Queue pairs, signalling and the congestion controller belong to the
   * provider. A report built only from the engine's settings describes a
   * configuration nobody is running -- which is how the first version of this
   * dump came to say cc=off during a run using an adaptive controller. */
  ObsFixture f;
  CHECK(f.setup());

  std::string const full = f.engine->describe();
  CHECK(contains(full, "\"engine\":"));
  CHECK(contains(full, "\"provider\":"));
  CHECK(contains(full, "\"provider\":\"mock\""));
  /* A provider-side setting the engine knows nothing about. */
  CHECK(contains(full, "\"move_data\":"));
}
