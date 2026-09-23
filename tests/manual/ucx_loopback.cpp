/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The UCX provider end to end, in one process.
 *
 * One engine is its own peer, with the progress thread running, so the
 * caller submits while that thread polls -- two threads on one UCX worker,
 * which is what the provider has to serialize. Writes and reads at several
 * sizes, each checked the moment it reports done: a write reported before
 * its flush, or a read reported by a request reused from an earlier
 * operation, shows up as bytes that are not there yet.
 *
 * The remote key is looked up in the provider's own registrations, so both
 * ends have to be one provider -- see docs/ucx.md.
 *
 *   UCX_TLS=self,sm ./ucx_loopback [rounds]
 *   UCX_TLS=rc,ud UCX_NET_DEVICES=<dev>:1 ./ucx_loopback [rounds] */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "transport/ucx/ucx_provider.h"

using namespace hux;

namespace {

uint8_t stamp(int round, uint64_t i) {
  return static_cast<uint8_t>((round * 131 + i * 7 + 1) & 0xff);
}

}  // namespace

int main(int argc, char** argv) {
  int const rounds = argc > 1 ? std::atoi(argv[1]) : 2000;
  uint64_t const sizes[] = {64, 4096, 65536, 1u << 20, 4u << 20};
  uint64_t const max_bytes = 4u << 20;

  std::shared_ptr<UcxProvider> prov;
  if (UcxProvider::create(UcxConfig{}, &prov) != Status::kOk) {
    std::printf("UCX provider did not start\n");
    return 1;
  }
  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread;
  std::unique_ptr<Engine> engine;
  if (make_engine(cfg, nullptr, prov, &engine) != Status::kOk) {
    std::printf("engine did not start\n");
    return 1;
  }

  std::vector<uint8_t> src(max_bytes), dst(max_bytes, 0), back(max_bytes, 0);
  MemoryRegionPtr rsrc, rdst, rback;
  AccessFlags const all = AccessFlags::kLocalRead | AccessFlags::kLocalWrite |
                          AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite;
  if (engine->register_memory(src.data(), max_bytes, all, &rsrc) !=
          Status::kOk ||
      engine->register_memory(dst.data(), max_bytes, all, &rdst) !=
          Status::kOk ||
      engine->register_memory(back.data(), max_bytes, all, &rback) !=
          Status::kOk) {
    std::printf("registration failed\n");
    return 1;
  }
  std::vector<uint8_t> meta, desc;
  PeerPtr peer;
  RemoteRegionPtr remote;
  if (engine->local_metadata(&meta) != Status::kOk ||
      engine->add_peer(meta, &peer) != Status::kOk ||
      rdst->export_descriptor(&desc) != Status::kOk ||
      peer->import_region(desc, &remote) != Status::kOk) {
    std::printf("could not reach itself\n");
    return 1;
  }

  int wrong = 0, failed = 0;
  for (int round = 0; round < rounds; ++round) {
    uint64_t const n = sizes[round % (sizeof(sizes) / sizeof(*sizes))];
    for (uint64_t i = 0; i < n; ++i) src[i] = stamp(round, i);

    RegionView ls, rd, lb;
    rsrc->view(0, n, &ls);
    remote->view(0, n, &rd);
    rback->view(0, n, &lb);

    RequestPtr w;
    if (engine->write(peer.get(), ls, rd, {}, &w) != Status::kOk ||
        w->wait(10000) != Status::kOk) {
      ++failed;
      continue;
    }
    /* Done means the bytes are there: the target is this process's memory,
     * so it can be looked at the moment the write says so. */
    if (std::memcmp(dst.data(), src.data(), n) != 0) {
      if (wrong < 5)
        std::printf("round %d: %llu B write reported done before it landed\n",
                    round, (unsigned long long)n);
      ++wrong;
    }

    std::memset(back.data(), 0, n);
    RequestPtr r;
    if (engine->read(peer.get(), lb, rd, {}, &r) != Status::kOk ||
        r->wait(10000) != Status::kOk) {
      ++failed;
      continue;
    }
    if (std::memcmp(back.data(), src.data(), n) != 0) {
      if (wrong < 5)
        std::printf("round %d: %llu B read reported done before it landed\n",
                    round, (unsigned long long)n);
      ++wrong;
    }
  }

  std::printf("%d rounds: %d wrong, %d failed\n%s\n", rounds, wrong, failed,
              engine->describe().c_str());
  return wrong == 0 && failed == 0 ? 0 : 1;
}
