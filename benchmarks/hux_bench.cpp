/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Transfer benchmark.
 *
 * Timing starts when the request is submitted and stops when it reaches a
 * terminal state -- the whole path the caller waits on, not the network stage
 * alone. A figure that covers only the wire looks better and answers a
 * question nobody asked.
 *
 * Each point reports a median and the spread around it. A single number hides
 * whether a run was steady or merely lucky once.
 *
 *   ./hux-bench server [--qp N] [--cc SPEC]
 *   ./hux-bench client <ip> [--qp N] [--cc SPEC] [--sizes a,b,c] [--iters N]
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "core/region_impl.h"
#include "hux/engine.h"
#include "transport/cc/controller.h"
#include "transport/rdma/rdma_provider.h"

using namespace hux;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint16_t kMetaPort = 18516;

struct Options {
  uint32_t qp = 1;
  std::string cc = "off";
  /* The address peers dial back on, which across machines also selects the
   * port and the GID. It has to be the address the kernel routes to the peer:
   * a host with two ports on one subnet will otherwise connect and then fail
   * every transfer. */
  std::string local_ip = "0.0.0.0";
  /* 0 leaves the engine's default. Sweeping it is how a caller finds out
   * whether tuning is worth doing at all. */
  uint64_t chunk = 0;
  std::vector<uint64_t> sizes = {4096, 65536, 1u << 20, 8u << 20};
  int iters = 50;
  int warmup = 5;
};

bool send_blob(int fd, void const* p, uint32_t n) {
  uint32_t len = htonl(n);
  if (::send(fd, &len, 4, 0) != 4) return false;
  auto const* b = static_cast<uint8_t const*>(p);
  uint32_t left = n;
  while (left > 0) {
    ssize_t k = ::send(fd, b, left, 0);
    if (k <= 0) return false;
    b += k;
    left -= static_cast<uint32_t>(k);
  }
  return true;
}

bool recv_blob(int fd, std::vector<uint8_t>* out) {
  uint32_t len = 0;
  if (::recv(fd, &len, 4, MSG_WAITALL) != 4) return false;
  len = ntohl(len);
  out->assign(len, 0);
  uint32_t got = 0;
  while (got < len) {
    ssize_t k = ::recv(fd, out->data() + got, len - got, 0);
    if (k <= 0) return false;
    got += static_cast<uint32_t>(k);
  }
  return true;
}

CongestionControllerPtr make_cc(std::string const& spec) {
  if (spec == "timely") return make_cc_timely();
  if (spec.rfind("fixed:", 0) == 0)
    return make_cc_fixed_window(std::strtoull(spec.c_str() + 6, nullptr, 10));
  return make_cc_off();
}

struct Summary {
  double median_us = 0;
  double p10_us = 0;
  double p90_us = 0;
  double gbps = 0;
};

Summary summarize(std::vector<double> us, uint64_t bytes) {
  Summary s;
  if (us.empty()) return s;
  std::sort(us.begin(), us.end());
  auto at = [&](double q) {
    size_t i = static_cast<size_t>(q * (us.size() - 1));
    return us[i];
  };
  s.median_us = at(0.5);
  s.p10_us = at(0.1);
  s.p90_us = at(0.9);
  /* Derived from the median, so an outlier cannot inflate it. */
  s.gbps = s.median_us > 0 ? (bytes * 8.0) / (s.median_us * 1e3) : 0;
  return s;
}

int run_server(Options const& o) {
  RdmaConfig cfg;
  cfg.advertise_ip = o.local_ip;
  cfg.qp_per_conn = o.qp;
  cfg.cc = make_cc(o.cc);
  std::shared_ptr<RdmaProvider> prov;
  if (RdmaProvider::create(cfg, &prov) != Status::kOk) {
    std::printf("provider create failed\n");
    return 1;
  }

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  if (o.chunk > 0) ecfg.chunk_bytes = o.chunk;
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, nullptr, prov, &engine) != Status::kOk) return 1;

  uint64_t const biggest =
      *std::max_element(o.sizes.begin(), o.sizes.end());
  std::vector<uint8_t> pool(biggest, 0x5a);
  MemoryRegionPtr region;
  if (engine->register_memory(pool.data(), pool.size(),
                              AccessFlags::kRemoteRead |
                                  AccessFlags::kRemoteWrite,
                              &region) != Status::kOk) {
    std::printf("register failed\n");
    return 1;
  }
  std::vector<uint8_t> desc;
  region->export_descriptor(&desc);
  std::vector<uint8_t> meta;
  prov->local_metadata(&meta);

  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = INADDR_ANY;
  a.sin_port = htons(kMetaPort);
  ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  ::listen(srv, 1);
  std::printf("[server] ready on :%u, %llu MiB registered\n", kMetaPort,
              (unsigned long long)(pool.size() >> 20));
  int fd = ::accept(srv, nullptr, nullptr);

  send_blob(fd, meta.data(), static_cast<uint32_t>(meta.size()));
  send_blob(fd, desc.data(), static_cast<uint32_t>(desc.size()));

  ProviderConnectionPtr conn;
  if (prov->accept(30000, &conn) != Status::kOk) {
    std::printf("[server] rdma accept failed\n");
    return 1;
  }
  std::printf("[server] connected; holding memory until the client finishes\n");

  std::vector<uint8_t> done;
  recv_blob(fd, &done);
  ::close(fd);
  ::close(srv);
  std::printf("[server] done\n");
  return 0;
}

int run_client(std::string const& ip, Options const& o) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kMetaPort);
  ::inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
  for (int i = 0; i < 30; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) break;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::vector<uint8_t> meta, desc;
  recv_blob(fd, &meta);
  recv_blob(fd, &desc);

  /* The server advertises 0.0.0.0; dial the address it was actually reached
   * on. */
  std::vector<uint8_t> fixed(meta.begin(), meta.begin() + 4);
  fixed.push_back(static_cast<uint8_t>(ip.size() & 0xff));
  fixed.push_back(static_cast<uint8_t>(ip.size() >> 8));
  fixed.insert(fixed.end(), ip.begin(), ip.end());

  RdmaConfig cfg;
  /* This side's own address, not the peer's: it selects the local port and
   * GID. */
  cfg.advertise_ip = o.local_ip;
  cfg.qp_per_conn = o.qp;
  cfg.cc = make_cc(o.cc);
  std::shared_ptr<RdmaProvider> prov;
  if (RdmaProvider::create(cfg, &prov) != Status::kOk) return 1;

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  if (o.chunk > 0) ecfg.chunk_bytes = o.chunk;
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, nullptr, prov, &engine) != Status::kOk) return 1;

  uint64_t const biggest = *std::max_element(o.sizes.begin(), o.sizes.end());
  std::vector<uint8_t> pool(biggest, 0);
  MemoryRegionPtr local;
  if (engine->register_memory(pool.data(), pool.size(),
                              AccessFlags::kLocalRead | AccessFlags::kLocalWrite,
                              &local) != Status::kOk)
    return 1;

  PeerPtr peer;
  if (engine->add_peer(fixed, &peer) != Status::kOk) {
    std::printf("add_peer failed\n");
    return 1;
  }
  RemoteRegionPtr remote;
  if (peer->import_region(desc, &remote) != Status::kOk) return 1;

  std::printf("\n%s\n\n", engine->describe().c_str());
  std::printf("%-10s %-8s %10s %10s %10s %10s\n", "size", "op", "median_us",
              "p10_us", "p90_us", "Gb/s");

  auto one_transfer = [&](uint64_t bytes, bool write) -> double {
    RegionView lv, rv;
    if (local->view(0, bytes, &lv) != Status::kOk) return -1;
    if (remote->view(0, bytes, &rv) != Status::kOk) return -1;

    auto const t0 = Clock::now();
    RequestPtr req;
    Status s = write ? engine->write(peer.get(), lv, rv, {}, &req)
                     : engine->read(peer.get(), lv, rv, {}, &req);
    if (s != Status::kOk) return -1;

    std::vector<RequestPtr> done;
    while (!is_terminal(req->state())) engine->poll_completions(64, &done);
    auto const t1 = Clock::now();
    if (req->state() != RequestState::kSucceeded) return -1;
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
  };

  for (uint64_t bytes : o.sizes) {
    /* Said out loud. A size that cannot run used to print as 0.0 us and
     * 0 Gb/s, which reads as a measurement rather than as a size that was
     * never attempted -- and the usual cause is the two sides having been
     * started with different --sizes, which nothing else would reveal. */
    if (bytes > pool.size()) {
      std::printf("%-10llu %-8s  skipped: larger than this side's buffer\n",
                  (unsigned long long)bytes, "-");
      continue;
    }
    if (bytes > remote->length()) {
      std::printf(
          "%-10llu %-8s  skipped: larger than the peer's region (%llu B)."
          " Start the server with the same --sizes.\n",
          (unsigned long long)bytes, "-",
          (unsigned long long)remote->length());
      continue;
    }
    for (bool write : {false, true}) {
      /* Discarded: the first transfers pay for connection setup and page
       * faults, which are real costs but not the one being measured. */
      for (int i = 0; i < o.warmup; ++i) one_transfer(bytes, write);

      std::vector<double> samples;
      samples.reserve(o.iters);
      for (int i = 0; i < o.iters; ++i) {
        double us = one_transfer(bytes, write);
        if (us >= 0) samples.push_back(us);
      }
      if (samples.empty()) {
        std::printf("%-10llu %-8s  no transfer succeeded\n",
                    (unsigned long long)bytes, write ? "write" : "read");
        continue;
      }
      Summary const sum = summarize(samples, bytes);
      std::printf("%-10llu %-8s %10.1f %10.1f %10.1f %10.2f\n",
                  (unsigned long long)bytes, write ? "write" : "read",
                  sum.median_us, sum.p10_us, sum.p90_us, sum.gbps);
    }
  }

  EngineStats const st = engine->stats();
  std::printf("\nextra payload copied: %llu B%s\n",
              (unsigned long long)st.payload_bytes_copied,
              st.payload_bytes_copied == 0 ? "   (in place)" : "");
  std::printf("sub-operations posted=%llu completed=%llu failed=%llu\n",
              (unsigned long long)st.subops_posted,
              (unsigned long long)st.subops_completed,
              (unsigned long long)st.subops_failed);

  send_blob(fd, "x", 1);
  ::close(fd);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s server|client <ip> [--qp N] [--cc SPEC]"
                " [--sizes a,b,c] [--iters N] [--local IP] [--chunk BYTES]\n",
                argv[0]);
    return 2;
  }
  Options o;
  for (int i = 1; i + 1 < argc; ++i) {
    std::string const k = argv[i];
    if (k == "--qp") o.qp = static_cast<uint32_t>(std::atoi(argv[i + 1]));
    else if (k == "--local") o.local_ip = argv[i + 1];
    else if (k == "--chunk")
      o.chunk = std::strtoull(argv[i + 1], nullptr, 10);
    else if (k == "--cc") o.cc = argv[i + 1];
    else if (k == "--iters") o.iters = std::atoi(argv[i + 1]);
    else if (k == "--sizes") {
      o.sizes.clear();
      char* s = argv[i + 1];
      for (char* tok = std::strtok(s, ","); tok; tok = std::strtok(nullptr, ","))
        o.sizes.push_back(std::strtoull(tok, nullptr, 10));
    }
  }

  if (std::strcmp(argv[1], "server") == 0) return run_server(o);
  if (argc < 3) return 2;
  return run_client(argv[2], o);
}
