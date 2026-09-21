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
#include <map>
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

/* Device memory, when a backend was built in. Without one this benchmark
 * measures host memory, which is a different thing from what a GPU
 * application transfers and must not be reported as if it were the same. */
#ifdef HUX_BENCH_CUDA
#include <cuda_runtime.h>

#include "device/cuda_backend.h"
#endif
#ifdef HUX_BENCH_ROCM
#include <hip/hip_runtime.h>

#include "device/rocm_backend.h"
#endif
#ifdef HUX_BENCH_NEUWARE
#include <cnrt.h>

#include "device/neuware_backend.h"
#endif

using namespace hux;
using Clock = std::chrono::steady_clock;

namespace {

/* Overridable so several flows can run at once on one pair of machines,
 * which is the only way to put this fabric under contention and the only
 * condition where congestion control can be worth anything. */
uint16_t g_meta_port = 18516;

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
  /* Negative keeps host memory; otherwise the device whose memory moves. */
  int gpu = -1;
  /* Requests kept outstanding. One keeps a flow latency-bound and cannot
   * saturate a link. */
  int inflight = 1;
  /* Sizes to interleave in one stream, so that a short request has a long
   * one ahead of it. Empty means each size runs on its own. */
  std::vector<uint64_t> mix;
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
  /* timely:<Mb/s> overrides only the additive increase. The default follows
   * the paper, which was written against a 10GE fabric; on a faster one the
   * ramp from the starting rate to line rate takes thousands of completions,
   * and a run shorter than that measures the ramp rather than the
   * controller. Made settable so that is a measurement and not an
   * assertion. */
  if (spec.rfind("timely:", 0) == 0) {
    TimelyParams p;
    /* Given in Mb/s, stored in bytes per second. */
    p.additive_increase_Bps =
        std::strtod(spec.c_str() + 7, nullptr) * 1e6 / 8.0;
    return make_cc_timely(p);
  }
  if (spec.rfind("fixed:", 0) == 0)
    return make_cc_fixed_window(std::strtoull(spec.c_str() + 6, nullptr, 10));
  return make_cc_off();
}

/* Host or device memory, so the same benchmark can measure what an
 * application would actually move. */
struct Pool {
  std::shared_ptr<DeviceBackend> dev;
  std::vector<uint8_t> host;
  void* ptr = nullptr;
  uint64_t bytes = 0;

  bool make(int gpu, uint64_t n) {
    bytes = n;
    if (gpu < 0) {
      host.assign(n, 0x5a);
      ptr = host.data();
      return true;
    }
#if defined(HUX_BENCH_CUDA)
    if (CudaBackend::create(gpu, &dev) != Status::kOk) return false;
    if (cudaSetDevice(gpu) != cudaSuccess || cudaMalloc(&ptr, n) != cudaSuccess)
      return false;
#elif defined(HUX_BENCH_ROCM)
    if (RocmBackend::create(gpu, &dev) != Status::kOk) return false;
    if (hipSetDevice(gpu) != hipSuccess || hipMalloc(&ptr, n) != hipSuccess)
      return false;
#elif defined(HUX_BENCH_NEUWARE)
    if (NeuwareBackend::create(gpu, &dev) != Status::kOk) return false;
    if (cnrtSetDevice(gpu) != cnrtSuccess || cnrtMalloc(&ptr, n) != cnrtSuccess)
      return false;
#else
    (void)gpu;
    std::printf("built without a device backend; --gpu is unavailable\n");
    return false;
#endif
    return dev != nullptr;
  }
};

struct Summary {
  double median_us = 0;
  double p10_us = 0;
  double p90_us = 0;
  double p99_us = 0;
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
  s.p99_us = at(0.99);
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
  uint64_t const biggest =
      *std::max_element(o.sizes.begin(), o.sizes.end());
  Pool pool;
  if (!pool.make(o.gpu, biggest)) {
    std::printf("allocation failed\n");
    return 1;
  }

  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, pool.dev, prov, &engine) != Status::kOk) return 1;

  MemoryRegionPtr region;
  if (engine->register_memory(pool.ptr, pool.bytes,
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
  a.sin_port = htons(g_meta_port);
  ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  ::listen(srv, 1);
  std::printf("[server] ready on :%u, %llu MiB registered\n", g_meta_port,
              (unsigned long long)(pool.bytes >> 20));
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
  a.sin_port = htons(g_meta_port);
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
  uint64_t const biggest = *std::max_element(o.sizes.begin(), o.sizes.end());
  Pool pool;
  if (!pool.make(o.gpu, biggest)) {
    std::printf("allocation failed\n");
    return 1;
  }

  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, pool.dev, prov, &engine) != Status::kOk) return 1;

  MemoryRegionPtr local;
  if (engine->register_memory(pool.ptr, pool.bytes,
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
  std::printf("%-10s %-8s %10s %10s %10s %10s %10s\n", "size", "op",
              "median_us", "p10_us", "p90_us", "Gb/s", "rate_Gb/s");

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

  /* Runs a schedule of sizes with `depth` requests outstanding rather than
   * one, and reports the latencies separately for each size.
   *
   * Depth does not buy throughput on this fabric -- one outstanding 4 MiB
   * write already reaches 98% of the link, and 8 or 32 add about 2% while
   * multiplying latency by the depth. What it buys is a queue, which is the
   * condition a congestion controller exists for; and a schedule of mixed
   * sizes is what puts a short request behind a long one, which is the
   * particular harm it is supposed to prevent.
   *
   * Returns the wall time as well, since with requests overlapping the
   * median no longer implies the rate. */
  using Runs = std::map<uint64_t, std::vector<double>>;
  auto run_schedule = [&](std::vector<uint64_t> const& schedule, bool write,
                          int depth) -> std::pair<Runs, double> {
    Runs samples;
    /* One view per distinct size, prepared before the clock starts so that
     * building them is not counted as transfer time. */
    std::map<uint64_t, std::pair<RegionView, RegionView>> views;
    for (uint64_t bytes : schedule) {
      if (views.count(bytes) != 0) continue;
      RegionView lv, rv;
      if (local->view(0, bytes, &lv) != Status::kOk) return {samples, 0.0};
      if (remote->view(0, bytes, &rv) != Status::kOk) return {samples, 0.0};
      views[bytes] = {lv, rv};
      samples[bytes];
    }

    struct Live {
      RequestPtr req;
      Clock::time_point at;
      uint64_t bytes;
    };
    std::vector<Live> live;
    size_t submitted = 0, finished = 0;
    auto const start = Clock::now();

    while (finished < schedule.size()) {
      while (static_cast<int>(live.size()) < depth &&
             submitted < schedule.size()) {
        uint64_t const bytes = schedule[submitted];
        auto const& v = views[bytes];
        RequestPtr req;
        auto const at = Clock::now();
        Status s = write ? engine->write(peer.get(), v.first, v.second, {}, &req)
                         : engine->read(peer.get(), v.first, v.second, {}, &req);
        /* Would-block is the engine saying its queue is full, which is the
         * normal way a deep pipeline finds the bottom. Not an error: stop
         * adding and let completions make room. */
        if (s != Status::kOk) break;
        live.push_back({std::move(req), at, bytes});
        ++submitted;
      }
      std::vector<RequestPtr> done;
      engine->poll_completions(64, &done);
      auto const now = Clock::now();
      for (auto it = live.begin(); it != live.end();) {
        if (!is_terminal(it->req->state())) {
          ++it;
          continue;
        }
        if (it->req->state() == RequestState::kSucceeded)
          samples[it->bytes].push_back(
              std::chrono::duration<double, std::micro>(now - it->at).count());
        ++finished;
        it = live.erase(it);
      }
      if (live.empty() && submitted >= schedule.size()) break;
    }
    double const wall =
        std::chrono::duration<double>(Clock::now() - start).count();
    return {samples, wall};
  };

  auto pipelined = [&](uint64_t bytes, bool write, int depth,
                       int count) -> std::pair<std::vector<double>, double> {
    auto const [runs, wall] =
        run_schedule(std::vector<uint64_t>(count, bytes), write, depth);
    auto it = runs.find(bytes);
    return {it == runs.end() ? std::vector<double>{} : it->second, wall};
  };

  if (!o.mix.empty()) {
    /* Each size alone first, then all of them interleaved. Without the
     * alone column there is nothing to compare the mixed one against, and
     * "a short request took N us behind a long one" says nothing about
     * whether it was held up. */
    uint64_t const cap = std::min<uint64_t>(pool.bytes, remote->length());
    std::vector<uint64_t> sizes;
    for (uint64_t b : o.mix)
      if (b <= cap) sizes.push_back(b);
    if (sizes.size() != o.mix.size())
      std::printf("\nmix: %zu of %zu sizes fit in both regions\n",
                  sizes.size(), o.mix.size());

    if (sizes.size() >= 2) {
      std::printf("\nmixed stream, %d in flight, write\n", o.inflight);
      std::printf("%-10s %12s %12s %12s %12s\n", "size", "alone_p50",
                  "mixed_p50", "alone_p99", "mixed_p99");

      std::map<uint64_t, Summary> alone;
      for (uint64_t b : sizes) {
        for (int i = 0; i < o.warmup; ++i) one_transfer(b, true);
        auto const [runs, wall] = run_schedule(
            std::vector<uint64_t>(o.iters, b), true, o.inflight);
        (void)wall;
        alone[b] = summarize(runs.at(b), b);
      }

      /* Round robin, so the counts per size are equal and every long
       * request has short ones on both sides of it. */
      std::vector<uint64_t> schedule;
      schedule.reserve(o.iters * sizes.size());
      for (int i = 0; i < o.iters; ++i)
        for (uint64_t b : sizes) schedule.push_back(b);
      for (int i = 0; i < o.warmup; ++i)
        for (uint64_t b : sizes) one_transfer(b, true);
      auto const [mixed_runs, mixed_wall] =
          run_schedule(schedule, true, o.inflight);

      uint64_t total = 0;
      for (uint64_t b : sizes) {
        Summary const m = summarize(mixed_runs.at(b), b);
        total += mixed_runs.at(b).size() * b;
        std::printf("%-10llu %12.1f %12.1f %12.1f %12.1f\n",
                    (unsigned long long)b, alone[b].median_us, m.median_us,
                    alone[b].p99_us, m.p99_us);
      }
      std::printf("mixed stream rate: %.2f Gb/s\n",
                  mixed_wall > 0 ? total * 8.0 / (mixed_wall * 1e9) : 0.0);
    }
  }

  for (uint64_t bytes : o.sizes) {
    /* Said out loud. A size that cannot run used to print as 0.0 us and
     * 0 Gb/s, which reads as a measurement rather than as a size that was
     * never attempted -- and the usual cause is the two sides having been
     * started with different --sizes, which nothing else would reveal. */
    if (bytes > pool.bytes) {
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

      auto const [samples, wall] = pipelined(bytes, write, o.inflight, o.iters);
      if (samples.empty()) {
        std::printf("%-10llu %-8s  no transfer succeeded\n",
                    (unsigned long long)bytes, write ? "write" : "read");
        continue;
      }
      Summary const sum = summarize(samples, bytes);
      /* Rate from the wall clock over the whole run. With requests
       * overlapping, dividing one transfer's latency into its bytes
       * describes nothing. */
      double const rate =
          wall > 0 ? samples.size() * bytes * 8.0 / (wall * 1e9) : 0.0;
      std::printf("%-10llu %-8s %10.1f %10.1f %10.1f %10.2f %10.2f\n",
                  (unsigned long long)bytes, write ? "write" : "read",
                  sum.median_us, sum.p10_us, sum.p90_us, sum.gbps, rate);
    }
  }

  /* Printed again at the end, because a controller's state is the result of
   * the run, not of the configuration it started with. */
  std::printf("\nafter the run: %s\n", prov->describe().c_str());

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
                " [--sizes a,b,c] [--iters N] [--local IP] [--chunk BYTES]"
                " [--gpu N] [--port P] [--inflight N] [--mix a,b]\n",
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
    else if (k == "--gpu")
      o.gpu = std::atoi(argv[i + 1]);
    else if (k == "--inflight")
      o.inflight = std::max(1, std::atoi(argv[i + 1]));
    else if (k == "--mix") {
      o.mix.clear();
      char const* p = argv[i + 1];
      while (*p != '\0') {
        o.mix.push_back(std::strtoull(p, nullptr, 10));
        char const* c = std::strchr(p, ',');
        if (c == nullptr) break;
        p = c + 1;
      }
    }
    else if (k == "--port")
      g_meta_port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
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
