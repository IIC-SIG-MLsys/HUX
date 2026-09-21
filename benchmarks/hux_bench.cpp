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
#include <sys/resource.h>
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
  /* One address per adapter to use. More than one puts a transport on each
   * and splits transfers between them; the weights say in what proportion,
   * and an even split across unequal adapters is worse than using the better
   * one alone. */
  std::vector<std::string> local_ips;
  std::vector<double> weights;
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
  /* How many servers to talk to at once, on consecutive ports. One engine
   * with several peers is the shape a client actually takes, and it is a
   * shape nothing else here measures. */
  int peers = 1;
  /* Segments per transfer. Above one the request goes through the vector
   * path, which is a different code path from a single contiguous transfer
   * -- it pairs segments by index, submits them together, and has to handle
   * a partial submit. Nothing in this benchmark exercised it. */
  int segments = 1;
  /* A recorded workload to replay. One record per line:
   *     <at_us> <bytes> <r|w>
   * with blank lines and # comments ignored. Times are from the start of
   * the replay, so a trace describes when work arrives rather than how fast
   * this end can go. */
  std::string trace;
  /* Rounds of the producer-consumer check. Zero skips it. */
  int produce = 0;
  /* How much work to put on the stream before recording the event. Tuned so
   * the producer has not finished when the transfer is issued; too little
   * and both arms pass because there was no race to lose. */
  int produce_repeats = 400;
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

  /* Every peer stamps its memory with a byte of its own, so a reader can
   * check it got the peer it asked for. Nothing else in the benchmark
   * distinguishes one peer's bytes from another's, and a request landing on
   * the wrong peer's region succeeds with the right length. */
  bool fill(uint8_t v) {
    if (ptr == nullptr) return false;
    if (dev == nullptr) {
      std::memset(ptr, v, bytes);
      return true;
    }
    std::vector<uint8_t> pattern(bytes, v);
    return dev->copy(ptr, pattern.data(), bytes) == Status::kOk;
  }

  /* The first bytes back on the host, for that check. */
  bool peek(std::vector<uint8_t>* out, uint64_t n) const {
    if (ptr == nullptr) return false;
    n = n < bytes ? n : bytes;
    out->assign(n, 0);
    if (dev == nullptr) {
      std::memcpy(out->data(), ptr, n);
      return true;
    }
    return dev->copy(out->data(), ptr, n) == Status::kOk;
  }
};

/* Work enqueued on the caller's own stream, standing in for the kernel that
 * produced the data about to be sent. Repeated writes rather than one,
 * because a single fill of a few megabytes finishes faster than the host
 * takes to issue the transfer, and a race that never happens proves nothing
 * about the ordering meant to prevent it.
 *
 * A fill rather than a kernel of our own: this benchmark would otherwise
 * need device code compiled per vendor, and what is under test is whether
 * the transfer waits for work on a stream, not what that work computes.
 *
 * Returns false where the vendor built in has no asynchronous fill. */
bool enqueue_producer(Pool const& pool, void* native_stream, uint8_t value,
                      int repeats) {
#if defined(HUX_BENCH_CUDA)
  auto* st = static_cast<cudaStream_t>(native_stream);
  for (int i = 0; i < repeats; ++i) {
    uint8_t const v = (i + 1 == repeats) ? value : static_cast<uint8_t>(i);
    if (cudaMemsetAsync(pool.ptr, v, pool.bytes, st) != cudaSuccess)
      return false;
  }
  return true;
#elif defined(HUX_BENCH_ROCM)
  auto* st = static_cast<hipStream_t>(native_stream);
  for (int i = 0; i < repeats; ++i) {
    uint8_t const v = (i + 1 == repeats) ? value : static_cast<uint8_t>(i);
    if (hipMemsetAsync(pool.ptr, v, pool.bytes, st) != hipSuccess) return false;
  }
  return true;
#else
  (void)pool;
  (void)native_stream;
  (void)value;
  (void)repeats;
  return false;
#endif
}

bool make_native_stream(void** out) {
#if defined(HUX_BENCH_CUDA)
  cudaStream_t st = nullptr;
  if (cudaStreamCreate(&st) != cudaSuccess) return false;
  *out = st;
  return true;
#elif defined(HUX_BENCH_ROCM)
  hipStream_t st = nullptr;
  if (hipStreamCreate(&st) != hipSuccess) return false;
  *out = st;
  return true;
#else
  (void)out;
  return false;
#endif
}

/* One line of a recorded workload. */
struct TraceRecord {
  uint64_t at_us = 0;
  uint64_t bytes = 0;
  bool write = false;
};

/* Reads the whole trace before the clock starts: parsing while replaying
 * would charge the file's cost to the transport. Returns false and says
 * which line was wrong, rather than silently replaying a shorter workload
 * than the file describes. */
bool read_trace(std::string const& path, std::vector<TraceRecord>* out) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (f == nullptr) {
    std::printf("cannot open trace %s\n", path.c_str());
    return false;
  }
  char line[512];
  int lineno = 0;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    ++lineno;
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == '\n' || *p == '\0') continue;
    unsigned long long at = 0, bytes = 0;
    char dir = 0;
    if (std::sscanf(p, "%llu %llu %c", &at, &bytes, &dir) != 3 ||
        (dir != 'r' && dir != 'w')) {
      std::printf("trace %s line %d: expected \"<at_us> <bytes> <r|w>\"\n",
                  path.c_str(), lineno);
      std::fclose(f);
      return false;
    }
    out->push_back({at, bytes, dir == 'w'});
  }
  std::fclose(f);
  /* Arrival order, whatever order the file was in. */
  std::sort(out->begin(), out->end(),
            [](TraceRecord const& a, TraceRecord const& b) {
              return a.at_us < b.at_us;
            });
  return !out->empty();
}

/* One transport per adapter named on the command line. The first keeps the
 * name "rdma" so a single-adapter run is unchanged; the rest are siblings of
 * it, which is how the engine knows they are two ways to the same place
 * rather than two different places. */
Status make_rdma_providers(Options const& o, RdmaConfig base,
                           std::vector<TransportProviderPtr>* out,
                           std::shared_ptr<RdmaProvider>* first) {
  std::vector<std::string> ips = o.local_ips;
  if (ips.empty()) ips.push_back(o.local_ip);
  for (size_t i = 0; i < ips.size(); ++i) {
    RdmaConfig cfg = base;
    cfg.advertise_ip = ips[i];
    cfg.nic_ordinal = static_cast<uint32_t>(i);
    cfg.relative_capacity = i < o.weights.size() ? o.weights[i] : 1.0;
    std::shared_ptr<RdmaProvider> p;
    Status const s = RdmaProvider::create(cfg, &p);
    if (s != Status::kOk) {
      std::printf("adapter for %s: %s\n", ips[i].c_str(), to_string(s));
      return s;
    }
    if (i == 0) *first = p;
    out->push_back(std::move(p));
  }
  return Status::kOk;
}

/* The byte a peer on this port stamps its memory with. Never zero, so an
 * untouched destination cannot pass for a peer's data. */
uint8_t stamp_for(uint16_t port) {
  return static_cast<uint8_t>((port % 251) + 1);
}

/* Processor time charged to this process, user and system together and
 * across all its threads. Divided by the bytes moved it answers what a
 * transport costs to run, which wall-clock throughput does not: reaching
 * line rate on one core and reaching it on four are not the same result,
 * and a busy-polling progress loop is exactly how the difference hides. */
double cpu_seconds() {
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
  auto const secs = [](timeval const& t) {
    return t.tv_sec + t.tv_usec / 1e6;
  };
  return secs(ru.ru_utime) + secs(ru.ru_stime);
}

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
  cfg.qp_per_conn = o.qp;
  cfg.cc = make_cc(o.cc);
  std::vector<TransportProviderPtr> provs;
  std::shared_ptr<RdmaProvider> prov;
  if (make_rdma_providers(o, cfg, &provs, &prov) != Status::kOk) {
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
  if (make_engine(ecfg, pool.dev, provs, &engine) != Status::kOk) return 1;

  MemoryRegionPtr region;
  if (engine->register_memory(pool.ptr, pool.bytes,
                              AccessFlags::kRemoteRead |
                                  AccessFlags::kRemoteWrite,
                              &region) != Status::kOk) {
    std::printf("register failed\n");
    return 1;
  }
  if (!pool.fill(stamp_for(g_meta_port))) {
    std::printf("[server] could not stamp the pool\n");
    return 1;
  }
  std::vector<uint8_t> desc;
  region->export_descriptor(&desc);
  /* The engine's metadata, not the provider's dialling blob. The blob works
   * -- add_peer accepts one -- but it carries no identity, so the client
   * cannot check that the region descriptor it is handed came from this
   * engine. Going through the engine is what the real callers do. */
  std::vector<uint8_t> meta;
  if (engine->local_metadata(&meta) != Status::kOk) {
    std::printf("local_metadata failed\n");
    return 1;
  }

  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = INADDR_ANY;
  a.sin_port = htons(g_meta_port);
  ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  ::listen(srv, 1);
  std::printf("[server] ready on :%u, %llu MiB registered, stamped 0x%02x\n",
              g_meta_port, (unsigned long long)(pool.bytes >> 20),
              stamp_for(g_meta_port));
  int fd = ::accept(srv, nullptr, nullptr);

  send_blob(fd, meta.data(), static_cast<uint32_t>(meta.size()));
  send_blob(fd, desc.data(), static_cast<uint32_t>(desc.size()));

  /* One accept per transport, because the client dials every one it can
   * reach: a server that accepted on the first would leave the others'
   * handshakes unanswered and the client waiting on them. */
  std::vector<ProviderConnectionPtr> conns;
  for (auto& tp : provs) {
    ProviderConnectionPtr conn;
    auto* rp = static_cast<RdmaProvider*>(tp.get());
    Status const s = rp->accept(30000, &conn);
    if (s != Status::kOk) {
      std::printf("[server] rdma accept on %s failed: %s\n",
                  rp->caps().name.c_str(), to_string(s));
      return 1;
    }
    conns.push_back(std::move(conn));
  }
  std::printf("[server] connected; holding memory until the client finishes\n");

  std::vector<uint8_t> done;
  recv_blob(fd, &done);
  ::close(fd);
  ::close(srv);
  std::printf("[server] done\n");
  return 0;
}

/* One server's metadata and exported region, over the side channel it
 * publishes them on. The socket stays open: the server holds its memory
 * registered until the client closes it. */
int fetch_from(std::string const& ip, uint16_t port, std::vector<uint8_t>* meta,
               std::vector<uint8_t>* desc) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  ::inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
  bool connected = false;
  for (int i = 0; i < 30; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (!connected) {
    ::close(fd);
    return -1;
  }
  if (!recv_blob(fd, meta) || !recv_blob(fd, desc)) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int run_client(std::string const& ip, Options const& o) {
  std::vector<uint8_t> meta, desc;
  int fd = fetch_from(ip, g_meta_port, &meta, &desc);
  if (fd < 0) {
    std::printf("could not reach a server on :%u\n", g_meta_port);
    return 1;
  }


  RdmaConfig cfg;
  /* This side's own addresses, not the peer's: each selects a local port and
   * GID, and one transport is built per address. */
  cfg.qp_per_conn = o.qp;
  cfg.cc = make_cc(o.cc);
  std::vector<TransportProviderPtr> provs;
  std::shared_ptr<RdmaProvider> prov;
  if (make_rdma_providers(o, cfg, &provs, &prov) != Status::kOk) return 1;

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
  if (make_engine(ecfg, pool.dev, provs, &engine) != Status::kOk) return 1;

  MemoryRegionPtr local;
  if (engine->register_memory(pool.ptr, pool.bytes,
                              AccessFlags::kLocalRead | AccessFlags::kLocalWrite,
                              &local) != Status::kOk)
    return 1;

  PeerPtr peer;
  Status const added = engine->add_peer(meta, &peer);
  if (added != Status::kOk) {
    /* The usual cause is the server having advertised 0.0.0.0, which is
     * what it does without --local: there is no address in its metadata to
     * dial. Naming it beats the silent exit this used to be. */
    std::printf("add_peer failed: %s. Start the server with --local <ip>.\n",
                to_string(added));
    return 1;
  }
  RemoteRegionPtr remote;
  Status const imported = peer->import_region(desc, &remote);
  if (imported != Status::kOk) {
    std::printf("import_region failed: %s\n", to_string(imported));
    return 1;
  }

  std::printf("\n%s\n\n", engine->describe().c_str());
  std::printf("%-10s %-8s %10s %10s %10s %10s %10s %7s %10s\n", "size", "op",
              "median_us", "p10_us", "p90_us", "Gb/s", "rate_Gb/s", "cores",
              "cpu_s/GiB");

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
  /* Set by every schedule run, read by whoever reports it. */
  double last_cpu_seconds = 0, last_wall_seconds = 0;

  using Runs = std::map<uint64_t, std::vector<double>>;
  auto run_schedule = [&](std::vector<uint64_t> const& schedule, bool write,
                          int depth) -> std::pair<Runs, double> {
    Runs samples;
    /* One view per distinct size, prepared before the clock starts so that
     * building them is not counted as transfer time. */
    std::map<uint64_t, std::pair<RegionView, RegionView>> views;
    /* And, when asked for, the same bytes as several segments. Scattered
     * rather than adjacent: consecutive pieces of one range would coalesce
     * in the adapter and the vector path would be measured doing the work
     * of a contiguous one. */
    std::map<uint64_t, std::pair<std::vector<RegionView>,
                                 std::vector<RegionView>>>
        segmented;
    for (uint64_t bytes : schedule) {
      if (views.count(bytes) != 0) continue;
      RegionView lv, rv;
      if (local->view(0, bytes, &lv) != Status::kOk) return {samples, 0.0};
      if (remote->view(0, bytes, &rv) != Status::kOk) return {samples, 0.0};
      views[bytes] = {lv, rv};
      samples[bytes];

      if (o.segments > 1) {
        uint64_t const n = static_cast<uint64_t>(o.segments);
        uint64_t const piece = bytes / n;
        /* A stride wider than a piece, so the segments do not touch. Needs
         * room for the last one to land inside the region. */
        uint64_t const stride = std::min<uint64_t>(
            piece * 2, piece == 0 ? 0 : (pool.bytes - piece) / (n > 1 ? n - 1 : 1));
        if (piece == 0 || stride < piece) {
          std::printf("%-10llu  skipped: %d segments do not fit\n",
                      (unsigned long long)bytes, o.segments);
          continue;
        }
        std::vector<RegionView> ls, rs;
        bool ok = true;
        for (uint64_t i = 0; i < n && ok; ++i) {
          RegionView a, b;
          if (local->view(i * stride, piece, &a) != Status::kOk ||
              remote->view(i * stride, piece, &b) != Status::kOk) {
            ok = false;
            break;
          }
          ls.push_back(a);
          rs.push_back(b);
        }
        if (ok) segmented[bytes] = {std::move(ls), std::move(rs)};
      }
    }
    if (o.segments > 1)
      for (uint64_t bytes : schedule)
        if (segmented.count(bytes) == 0) return {samples, 0.0};

    struct Live {
      RequestPtr req;
      Clock::time_point at;
      uint64_t bytes;
    };
    std::vector<Live> live;
    size_t submitted = 0, finished = 0;
    double const cpu0 = cpu_seconds();
    auto const start = Clock::now();

    while (finished < schedule.size()) {
      while (static_cast<int>(live.size()) < depth &&
             submitted < schedule.size()) {
        uint64_t const bytes = schedule[submitted];
        auto const& v = views[bytes];
        RequestPtr req;
        auto const at = Clock::now();
        Status s;
        if (o.segments > 1) {
          auto const& seg = segmented[bytes];
          s = write ? engine->writev(peer.get(), seg.first, seg.second, {}, &req)
                    : engine->readv(peer.get(), seg.first, seg.second, {}, &req);
        } else {
          s = write ? engine->write(peer.get(), v.first, v.second, {}, &req)
                    : engine->read(peer.get(), v.first, v.second, {}, &req);
        }
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
    last_cpu_seconds = cpu_seconds() - cpu0;
    last_wall_seconds = wall;
    return {samples, wall};
  };

  auto pipelined = [&](uint64_t bytes, bool write, int depth,
                       int count) -> std::pair<std::vector<double>, double> {
    auto const [runs, wall] =
        run_schedule(std::vector<uint64_t>(count, bytes), write, depth);
    auto it = runs.find(bytes);
    return {it == runs.end() ? std::vector<double>{} : it->second, wall};
  };

  if (!o.trace.empty()) {
    std::vector<TraceRecord> trace;
    if (!read_trace(o.trace, &trace)) return 1;

    /* Anything the region cannot hold is dropped rather than silently
     * shrunk, because a replay that quietly changed the sizes is no longer
     * replaying that workload. */
    uint64_t const cap = std::min<uint64_t>(pool.bytes, remote->length());
    size_t dropped = 0;
    trace.erase(std::remove_if(trace.begin(), trace.end(),
                               [&](TraceRecord const& r) {
                                 bool const too_big = r.bytes > cap || r.bytes == 0;
                                 if (too_big) ++dropped;
                                 return too_big;
                               }),
                trace.end());
    if (dropped > 0)
      std::printf("\ntrace: %zu of %zu records do not fit in %llu B and were"
                  " dropped\n",
                  dropped, dropped + trace.size(), (unsigned long long)cap);

    if (trace.empty()) {
      std::printf("\ntrace: nothing left to replay\n");
    } else {
      uint64_t total = 0;
      for (auto const& r : trace) total += r.bytes;
      std::printf("\nreplaying %zu records, %.1f MiB, over %.3f s\n",
                  trace.size(), total / double(1 << 20),
                  trace.back().at_us / 1e6);

      /* What the replayer costs on its own, before any transfer. Waiting
       * for a due time has a granularity, and without measuring it every
       * microsecond of that granularity would be charged to the transport.
       * Same loop, same clock, nothing submitted. */
      std::vector<double> floor_us;
      {
        auto const dry_start = Clock::now();
        for (auto const& r : trace) {
          auto const due = dry_start + std::chrono::microseconds(r.at_us);
          while (Clock::now() < due) {
            if (due - Clock::now() > std::chrono::microseconds(50))
              std::this_thread::sleep_for(std::chrono::microseconds(20));
          }
          floor_us.push_back(
              std::chrono::duration<double, std::micro>(Clock::now() - due)
                  .count());
        }
      }
      Summary const fl = summarize(floor_us, 0);

      /* Lateness is the measurement. Throughput says how fast this end can
       * go; a replay asks whether it kept up with work that arrived on
       * somebody else's schedule, and the answer is how far behind each
       * request went out -- above the floor just measured. */
      std::vector<double> late_us, lat_us;
      std::vector<RequestPtr> live;
      auto const start = Clock::now();
      size_t issued = 0;
      bool ok = true;
      while (issued < trace.size() && ok) {
        TraceRecord const& r = trace[issued];
        auto const due = start + std::chrono::microseconds(r.at_us);
        /* Completions are collected while waiting, so a request that is due
         * later does not queue behind the polling. */
        while (Clock::now() < due) {
          std::vector<RequestPtr> done;
          engine->poll_completions(64, &done);
          /* Sleeping right up to the deadline would overshoot it by the
           * sleep's own granularity, which is what the floor above
           * measures; spin the last stretch instead. */
          if (due - Clock::now() > std::chrono::microseconds(50))
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        auto const now = Clock::now();
        late_us.push_back(
            std::chrono::duration<double, std::micro>(now - due).count());

        RegionView lv, rv;
        if (local->view(0, r.bytes, &lv) != Status::kOk ||
            remote->view(0, r.bytes, &rv) != Status::kOk) {
          ok = false;
          break;
        }
        RequestPtr req;
        Status const st =
            r.write ? engine->write(peer.get(), lv, rv, {}, &req)
                    : engine->read(peer.get(), lv, rv, {}, &req);
        if (st != Status::kOk) {
          std::printf("record %zu (%llu B) -> %s\n", issued,
                      (unsigned long long)r.bytes, to_string(st));
          ok = false;
          break;
        }
        live.push_back(std::move(req));
        ++issued;

        for (auto it = live.begin(); it != live.end();) {
          if (!is_terminal((*it)->state())) {
            ++it;
            continue;
          }
          lat_us.push_back(
              std::chrono::duration<double, std::micro>(Clock::now() - now)
                  .count());
          it = live.erase(it);
        }
      }
      while (!live.empty()) {
        std::vector<RequestPtr> done;
        engine->poll_completions(64, &done);
        for (auto it = live.begin(); it != live.end();)
          it = is_terminal((*it)->state()) ? live.erase(it) : ++it;
      }
      double const wall =
          std::chrono::duration<double>(Clock::now() - start).count();

      Summary const l = summarize(late_us, 0);
      std::printf("%-22s %10s %10s %10s\n", "behind schedule", "median", "p90",
                  "p99");
      std::printf("%-22s %9.1fus %9.1fus %9.1fus\n", "  replayer alone",
                  fl.median_us, fl.p90_us, fl.p99_us);
      std::printf("%-22s %9.1fus %9.1fus %9.1fus\n", "  replaying", l.median_us,
                  l.p90_us, l.p99_us);
      std::printf("issued %zu of %zu; wall %.3f s against the trace's %.3f s;"
                  " %.2f Gb/s\n",
                  issued, trace.size(), wall, trace.back().at_us / 1e6,
                  wall > 0 ? total * 8.0 / (wall * 1e9) : 0.0);
      std::printf("the first row is this benchmark's own cost of waiting for a"
                  " due time.\nOnly what the second row has above it belongs"
                  " to the transport.\n");
    }
  }

  if (o.produce > 0) {
    /* A producer on the caller's stream, then a transfer of what it wrote.
     *
     * Both arms are run, and the one without the event is the point: if the
     * transfer arrives correct whether or not it was told to wait, then the
     * producer finished too early and this measured nothing. A single arm
     * reporting "correct" would be indistinguishable from a test too easy
     * to fail. */
    void* native = nullptr;
    if (pool.dev == nullptr || !make_native_stream(&native)) {
      std::printf("\n--produce needs a device backend and a GPU;"
                  " skipping\n");
    } else {
      DeviceStreamPtr stream;
      Status const si = pool.dev->import_stream(native, &stream);
      if (si != Status::kOk) {
        std::printf("\nimport_stream -> %s; skipping --produce\n",
                    to_string(si));
      } else {
        uint64_t const size = std::min<uint64_t>(o.sizes.front(), pool.bytes);
        std::printf("\nproducer on the caller's stream, %llu B,"
                    " %d fills before the event, %d rounds\n",
                    (unsigned long long)size, o.produce_repeats, o.produce);

        /* Reads the remote back into a second local region, because the
         * pool itself is what the producer is overwriting. */
        std::vector<uint8_t> check_host(size, 0);
        MemoryRegionPtr check_reg;
        if (engine->register_memory(check_host.data(), size,
                                    AccessFlags::kLocalWrite,
                                    &check_reg) != Status::kOk) {
          std::printf("could not register the verification buffer\n");
        } else {
          for (int arm = 0; arm < 2; ++arm) {
            bool const ordered = arm == 0;
            int correct = 0, done = 0;
            double total_us = 0;
            for (int n = 0; n < o.produce; ++n) {
              uint8_t const want = static_cast<uint8_t>(0x40 + (n % 64));
              if (!enqueue_producer(pool, native, want, o.produce_repeats))
                break;
              DeviceEventPtr ev;
              if (engine->record_event(stream.get(), &ev) != Status::kOk) break;

              TransferOptions opts;
              if (ordered) opts.after.push_back(ev);

              RegionView lv, rv;
              if (local->view(0, size, &lv) != Status::kOk) break;
              if (remote->view(0, size, &rv) != Status::kOk) break;
              auto const t0 = Clock::now();
              RequestPtr w;
              if (engine->write(peer.get(), lv, rv, opts, &w) != Status::kOk)
                break;
              std::vector<RequestPtr> fin;
              while (!is_terminal(w->state())) engine->poll_completions(64,
                                                                        &fin);
              total_us +=
                  std::chrono::duration<double, std::micro>(Clock::now() - t0)
                      .count();
              if (w->state() != RequestState::kSucceeded) break;

              /* What actually landed on the peer. */
              RegionView cv;
              if (check_reg->view(0, size, &cv) != Status::kOk) break;
              RequestPtr r;
              if (engine->read(peer.get(), cv, rv, {}, &r) != Status::kOk)
                break;
              while (!is_terminal(r->state())) engine->poll_completions(64,
                                                                        &fin);
              if (r->state() != RequestState::kSucceeded) break;
              ++done;
              bool all = true;
              for (uint64_t i = 0; i < size; ++i)
                if (check_host[i] != want) {
                  all = false;
                  break;
                }
              if (all) ++correct;
            }
            std::printf("  %-12s %3d/%3d arrived with what the producer"
                        " wrote, %8.1f us each\n",
                        ordered ? "after=[ev]" : "no ordering", correct, done,
                        done > 0 ? total_us / done : 0.0);
          }
          std::printf("  the second line is the control: if it also reads"
                      " correct, the producer\n  finished before the"
                      " transfer was issued and this measured nothing --"
                      "\n  raise --produce-repeats until it does not.\n");
        }
      }
    }
  }

  if (o.peers > 1) {
    /* One engine, several peers, each stamping its memory with a byte of its
     * own. The point is the check at the end: a read from the wrong peer's
     * region succeeds and returns the right number of bytes, so only the
     * contents say anything is wrong. Region ids are handed out per engine
     * from one, so every peer has a region 1 for a request to land on. */
    uint64_t const size = std::min<uint64_t>(o.sizes.front(), pool.bytes);
    std::printf("\n%d peers on :%u..%u, %llu B each\n", o.peers, g_meta_port,
                static_cast<unsigned>(g_meta_port + o.peers - 1),
                (unsigned long long)size);
    std::printf("%-8s %-10s %10s %12s %14s\n", "peer", "port", "median_us",
                "rate_Gb/s", "read back");

    struct Extra {
      int fd = -1;
      uint16_t port = 0;
      PeerPtr peer;
      RemoteRegionPtr remote;
    };
    std::vector<Extra> all;
    /* The first one is the peer already connected above. */
    all.push_back({fd, g_meta_port, peer, remote});
    bool ok = true;
    for (int i = 1; i < o.peers && ok; ++i) {
      Extra e;
      e.port = static_cast<uint16_t>(g_meta_port + i);
      std::vector<uint8_t> m, d;
      e.fd = fetch_from(ip, e.port, &m, &d);
      if (e.fd < 0) {
        std::printf("  no server on :%u -- start one per peer\n", e.port);
        ok = false;
        break;
      }
      Status s2 = engine->add_peer(m, &e.peer);
      if (s2 != Status::kOk) {
        std::printf("  add_peer(:%u) -> %s\n", e.port, to_string(s2));
        ok = false;
        break;
      }
      s2 = e.peer->import_region(d, &e.remote);
      if (s2 != Status::kOk) {
        std::printf("  import_region(:%u) -> %s\n", e.port, to_string(s2));
        ok = false;
        break;
      }
      all.push_back(std::move(e));
    }

    if (ok) {
      RegionView lv;
      if (local->view(0, size, &lv) != Status::kOk) ok = false;
      for (size_t i = 0; ok && i < all.size(); ++i) {
        RegionView rv;
        if (all[i].remote->view(0, size, &rv) != Status::kOk) {
          ok = false;
          break;
        }
        std::vector<double> lat;
        auto const start = Clock::now();
        for (int n = 0; n < o.iters; ++n) {
          auto const t0 = Clock::now();
          RequestPtr req;
          if (engine->read(all[i].peer.get(), lv, rv, {}, &req) != Status::kOk)
            break;
          std::vector<RequestPtr> done;
          while (!is_terminal(req->state())) engine->poll_completions(64, &done);
          if (req->state() != RequestState::kSucceeded) break;
          lat.push_back(
              std::chrono::duration<double, std::micro>(Clock::now() - t0)
                  .count());
        }
        double const wall =
            std::chrono::duration<double>(Clock::now() - start).count();
        if (lat.empty()) {
          std::printf("%-8zu :%-9u  no transfer succeeded\n", i, all[i].port);
          continue;
        }
        Summary const sum = summarize(lat, size);
        /* What came back, against what that port stamps. This is the check
         * the whole section exists for. */
        std::vector<uint8_t> got;
        uint8_t const want = stamp_for(all[i].port);
        bool right = pool.peek(&got, 64);
        for (uint8_t b : got)
          if (b != want) right = false;
        /* The byte is printed, not just the verdict: three peers all
         * reading "correct" means nothing unless their stamps differ, and
         * showing them is quicker than working it out. */
        char seen[16] = "unreadable";
        if (!got.empty()) std::snprintf(seen, sizeof(seen), "0x%02x", got[0]);
        std::printf("%-8zu :%-9u %10.1f %12.2f   %s %s\n", i, all[i].port,
                    sum.median_us,
                    wall > 0 ? lat.size() * size * 8.0 / (wall * 1e9) : 0.0,
                    seen, right ? "ok" : "WRONG PEER");
        if (!right && !got.empty())
          std::printf("         wanted 0x%02x, read 0x%02x -- this peer's"
                      " region resolved to another peer's memory\n",
                      want, got[0]);
      }
    }
    for (size_t i = 1; i < all.size(); ++i)
      if (all[i].fd >= 0) {
        send_blob(all[i].fd, "x", 1);
        ::close(all[i].fd);
      }
  }

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
      std::printf("mixed stream rate: %.2f Gb/s, %.2f cores, %.3f cpu_s/GiB\n",
                  mixed_wall > 0 ? total * 8.0 / (mixed_wall * 1e9) : 0.0,
                  last_wall_seconds > 0 ? last_cpu_seconds / last_wall_seconds
                                        : 0.0,
                  total > 0 ? last_cpu_seconds / (total / double(1 << 30))
                            : 0.0);
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
      /* cores: processor time over wall time, so 1.00 is one core busy for
       * the whole run. cpu_s/GiB is the portable form, since it does not
       * change with how fast the link happens to be. */
      double const gib = samples.size() * bytes / double(1 << 30);
      double const cores =
          last_wall_seconds > 0 ? last_cpu_seconds / last_wall_seconds : 0.0;
      std::printf(
          "%-10llu %-8s %10.1f %10.1f %10.1f %10.2f %10.2f %7.2f %10.3f\n",
          (unsigned long long)bytes, write ? "write" : "read", sum.median_us,
          sum.p10_us, sum.p90_us, sum.gbps, rate, cores,
          gib > 0 ? last_cpu_seconds / gib : 0.0);
    }
  }

  /* Said plainly, because the two CPU columns are easy to read as something
   * they are not. */
  std::printf(
      "\ncores is processor time over wall time: this engine polls for"
      " completions, so\nabout 1.00 is the design rather than an overhead"
      " to remove -- a core traded for\nlatency. cpu_s/GiB therefore falls"
      " as the link gets faster, and comparing it\nbetween two runs at"
      " different rates compares the rates.\n");

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
                " [--gpu N] [--port P] [--inflight N] [--mix a,b]\n"
                "       [--peers N] [--segments N] [--produce N]\n"
                "       [--produce-repeats N] [--trace FILE]\n"
                "       --local takes a list: one adapter per address, with"
                " --weights w1,w2\n",
                argv[0]);
    return 2;
  }
  Options o;
  for (int i = 1; i + 1 < argc; ++i) {
    std::string const k = argv[i];
    if (k == "--qp") o.qp = static_cast<uint32_t>(std::atoi(argv[i + 1]));
    else if (k == "--local") {
      o.local_ips.clear();
      char const* q = argv[i + 1];
      while (*q != '\0') {
        char const* c = std::strchr(q, ',');
        o.local_ips.emplace_back(q, c == nullptr ? std::strlen(q)
                                                 : static_cast<size_t>(c - q));
        if (c == nullptr) break;
        q = c + 1;
      }
      if (!o.local_ips.empty()) o.local_ip = o.local_ips.front();
    } else if (k == "--weights") {
      o.weights.clear();
      char const* q = argv[i + 1];
      while (*q != '\0') {
        o.weights.push_back(std::strtod(q, nullptr));
        char const* c = std::strchr(q, ',');
        if (c == nullptr) break;
        q = c + 1;
      }
    }
    else if (k == "--chunk")
      o.chunk = std::strtoull(argv[i + 1], nullptr, 10);
    else if (k == "--gpu")
      o.gpu = std::atoi(argv[i + 1]);
    else if (k == "--inflight")
      o.inflight = std::max(1, std::atoi(argv[i + 1]));
    else if (k == "--peers")
      o.peers = std::max(1, std::atoi(argv[i + 1]));
    else if (k == "--segments")
      o.segments = std::max(1, std::atoi(argv[i + 1]));
    else if (k == "--trace") o.trace = argv[i + 1];
    else if (k == "--produce") o.produce = std::max(0, std::atoi(argv[i + 1]));
    else if (k == "--produce-repeats")
      o.produce_repeats = std::max(1, std::atoi(argv[i + 1]));
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
