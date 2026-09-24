/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Disconnecting a connection with work in flight, against this host's own
 * adapter: every posted operation must still be accounted for -- completed
 * or flushed -- and nothing may be posted afterwards. This is what the
 * engine relies on to call a departed peer's requests FailedSafe; while
 * disconnect() did nothing, the queue pairs stayed live and the adapter
 * went on moving bytes for requests already reported safe.
 *
 *   hux_rdma_disconnect <address of an RDMA-capable interface>
 */
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "transport/rdma/rdma_provider.h"

using namespace hux;

int main(int argc, char** argv) {
  std::string const ip = argc > 1 ? argv[1] : "127.0.0.1";
  RdmaConfig cfg;
  cfg.advertise_ip = ip;
  std::shared_ptr<RdmaProvider> a, b;
  if (RdmaProvider::create(cfg, &a) != Status::kOk ||
      RdmaProvider::create(cfg, &b) != Status::kOk) {
    std::printf("no RDMA provider on %s; skipping\n", ip.c_str());
    return 0;
  }
  std::vector<uint8_t> meta;
  b->local_metadata(&meta);
  ProviderConnectionPtr ca, cb;
  std::thread acc([&] { b->accept(20000, &cb); });
  Status const con = a->connect(meta, &ca);
  acc.join();
  if (con != Status::kOk || cb == nullptr) {
    std::printf("connect failed: %s\n", to_string(con));
    return 1;
  }

  constexpr uint64_t kOp = 256 << 10;
  constexpr uint32_t kOps = 256;
  std::vector<uint8_t> src(kOp * kOps, 0x5a), dst(kOp * kOps, 0);
  uint64_t lk_b = 0, rk_b = 0, lk_a = 0, rk_a = 0;
  if (b->register_region(src.data(), src.size(), DeviceId{},
                         AccessFlags::kRemoteRead, &lk_b,
                         &rk_b) != Status::kOk ||
      a->register_region(dst.data(), dst.size(), DeviceId{},
                         AccessFlags::kLocalWrite, &lk_a,
                         &rk_a) != Status::kOk) {
    std::printf("registration failed\n");
    return 1;
  }
  std::vector<SubOp> ops(kOps);
  for (uint32_t i = 0; i < kOps; ++i) {
    ops[i].kind = SubOp::Kind::kRead;
    ops[i].request = 1;
    ops[i].sub_id = i;
    ops[i].local_addr = dst.data() + i * kOp;
    ops[i].local_key = lk_a;
    ops[i].remote_addr = reinterpret_cast<uint64_t>(src.data()) + i * kOp;
    ops[i].remote_key = rk_b;
    ops[i].length = kOp;
  }
  SubmitResult const sr = a->submit(ca.get(), ops);
  Status const dis = a->disconnect(ca);

  uint32_t ok = 0, failed = 0;
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (ok + failed < sr.accepted &&
         std::chrono::steady_clock::now() < deadline) {
    std::vector<CompletionEvent> ev;
    a->poll(64, &ev);
    for (auto const& e : ev) (e.status == Status::kOk ? ok : failed)++;
  }
  SubmitResult const after = a->submit(ca.get(), {ops[0]});

  std::printf(
      "posted %u, disconnect %s: %u completed, %u flushed, %u unaccounted\n",
      sr.accepted, to_string(dis), ok, failed, sr.accepted - ok - failed);
  std::printf("a submit after disconnect: accepted %u (%s)\n", after.accepted,
              to_string(after.status));
  bool const pass = ok + failed == sr.accepted && after.accepted == 0;
  std::printf("%s\n", pass ? "PASS" : "FAIL");
  a->deregister_region(lk_a);
  b->deregister_region(lk_b);
  return pass ? 0 : 1;
}
