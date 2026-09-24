/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Host memory from alloc_host: aligned, whole, usable -- and honest about
 * how much of it the kernel put on huge pages. */
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "core/factory.h"
#include "hux/engine.h"
#include "hux/host_memory.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

/* Whether this machine hands out transparent huge pages on request. */
bool thp_on_request() {
  std::ifstream f("/sys/kernel/mm/transparent_hugepage/enabled");
  std::string s;
  std::getline(f, s);
  return s.find("[always]") != std::string::npos ||
         s.find("[madvise]") != std::string::npos;
}

}  // namespace

HUX_TEST(host_memory_comes_in_whole_aligned_huge_pages) {
  HostAllocation a;
  CHECK_STATUS(alloc_host(3u << 20, &a), Status::kOk);
  CHECK(a.ptr != nullptr);
  CHECK_EQ(a.bytes, uint64_t(4) << 20);
  CHECK_EQ(reinterpret_cast<uintptr_t>(a.ptr) % (2u << 20), uintptr_t(0));
  CHECK(a.huge_bytes <= a.bytes);
  auto const* p = static_cast<uint8_t const*>(a.ptr);
  bool zero = true;
  for (uint64_t i = 0; i < a.bytes; i += 4096) zero = zero && p[i] == 0;
  CHECK(zero);
  std::printf("       %llu of %llu MiB on huge pages (%s)\n",
              (unsigned long long)(a.huge_bytes >> 20),
              (unsigned long long)(a.bytes >> 20),
              thp_on_request() ? "granted on request" : "not offered here");
  free_host(a);
}

HUX_TEST(host_memory_refuses_nothing_and_nowhere) {
  HostAllocation a;
  CHECK_STATUS(alloc_host(0, &a), Status::kInvalidArgument);
  CHECK_STATUS(alloc_host(4096, nullptr), Status::kInvalidArgument);
  free_host(HostAllocation{}); /* a no-op, not a crash */
}

HUX_TEST(host_memory_registers_and_moves_data) {
  auto provider = std::make_shared<MockProvider>(MockConfig{});
  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  CHECK_STATUS(make_engine(cfg, nullptr, provider, &engine), Status::kOk);

  HostAllocation src, dst;
  CHECK_STATUS(alloc_host(1u << 20, &src), Status::kOk);
  CHECK_STATUS(alloc_host(1u << 20, &dst), Status::kOk);
  std::memset(src.ptr, 0x3c, src.bytes);

  MemoryRegionPtr rs, rd;
  CHECK_STATUS(engine->register_memory(src.ptr, src.bytes,
                                       AccessFlags::kRemoteRead, &rs),
               Status::kOk);
  CHECK_STATUS(engine->register_memory(dst.ptr, dst.bytes,
                                       AccessFlags::kLocalWrite, &rd),
               Status::kOk);
  std::vector<uint8_t> meta, desc;
  engine->local_metadata(&meta);
  PeerPtr peer;
  CHECK_STATUS(engine->add_peer(meta, &peer), Status::kOk);
  rs->export_descriptor(&desc);
  RemoteRegionPtr remote;
  CHECK_STATUS(peer->import_region(desc, &remote), Status::kOk);

  RegionView lv, rv;
  rd->view(0, dst.bytes, &lv);
  remote->view(0, src.bytes, &rv);
  RequestPtr req;
  CHECK_STATUS(engine->read(peer.get(), lv, rv, {}, &req), Status::kOk);
  std::vector<RequestPtr> done;
  for (int i = 0; i < 100 && !is_terminal(req->state()); ++i)
    engine->poll_completions(16, &done);
  CHECK(req->state() == RequestState::kSucceeded);
  CHECK_EQ(std::memcmp(src.ptr, dst.ptr, dst.bytes), 0);

  CHECK_STATUS(engine->deregister_memory(rs), Status::kOk);
  CHECK_STATUS(engine->deregister_memory(rd), Status::kOk);
  rs.reset();
  rd.reset();
  remote.reset();
  uint32_t released = 0;
  engine->release_cached_registrations(&released);
  free_host(src);
  free_host(dst);
}
