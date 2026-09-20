/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Two processes on one host, transferring through a mapping rather than a NIC.
 *
 * Run the server first, then the client. Host memory cannot be exported, so
 * this needs a device whose backend implements the IPC calls.
 *
 *   ./hux_ipc_pair server [--gpu N]
 *   ./hux_ipc_pair client [--gpu N] */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "transport/ipc/ipc_provider.h"

/* One program, whichever vendor was built in. The backend differs; nothing
 * below it does. */
#ifdef HUX_LOOPBACK_CUDA
#include <cuda_runtime.h>

#include "device/cuda_backend.h"
#endif
#ifdef HUX_LOOPBACK_ROCM
#include <hip/hip_runtime.h>

#include "device/rocm_backend.h"
#endif
#ifdef HUX_LOOPBACK_NEUWARE
#include <cnrt.h>

#include "device/neuware_backend.h"
#endif

using namespace hux;

namespace {

constexpr uint64_t kBytes = 4u << 20;
/* Overridable, and worth overriding. Two runs of this program on one host
 * sharing a port do not fail cleanly: the second server's bind is refused,
 * but a client can still reach the first one and then verify bytes a
 * different pair is overwriting. A soak run alongside a short test is
 * exactly that, and it shows up as rare, unreproducible corruption. */
uint16_t g_port = 18516;

/* Diagnostic, off by default. The soak run sees rare rounds where the tail of
 * a read still holds what this side wrote the round before -- as if the
 * peer's refill had not become visible across the process boundary by the
 * time it said it had. A synchronous copy is supposed to make that
 * impossible; this switch is how that assumption gets tested rather than
 * argued about. */
bool g_sync_after_fill = false;

/* Unconditional, for the diagnostic path below. */
void device_barrier_always() {
#if defined(HUX_LOOPBACK_CUDA)
  cudaDeviceSynchronize();
#elif defined(HUX_LOOPBACK_ROCM)
  hipDeviceSynchronize();
#elif defined(HUX_LOOPBACK_NEUWARE)
  cnrtSyncDevice();
#endif
}

void device_barrier() {
  if (!g_sync_after_fill) return;
#if defined(HUX_LOOPBACK_CUDA)
  cudaDeviceSynchronize();
#elif defined(HUX_LOOPBACK_ROCM)
  hipDeviceSynchronize();
#elif defined(HUX_LOOPBACK_NEUWARE)
  cnrtSyncDevice();
#endif
}

void send_blob(int fd, void const* p, uint32_t n) {
  ::send(fd, &n, 4, 0);
  if (n > 0) ::send(fd, p, n, 0);
}

bool recv_blob(int fd, std::vector<uint8_t>* out) {
  uint32_t n = 0;
  if (::recv(fd, &n, 4, MSG_WAITALL) != 4) return false;
  out->resize(n);
  return n == 0 || ::recv(fd, out->data(), n, MSG_WAITALL) == ssize_t(n);
}

/* Device memory plus the two things a test needs from it: filling it and
 * reading it back, both through the backend so no vendor call appears here. */
struct Buffer {
  std::shared_ptr<DeviceBackend> dev;
  void* ptr = nullptr;

  /* Built once and kept. A soak run does this tens of thousands of times, and
   * a pattern rebuilt per round would put the test's own arithmetic on the
   * critical path instead of the transfer. */
  std::vector<uint8_t> const& pattern(uint8_t seed) const {
    auto it = patterns.find(seed);
    if (it != patterns.end()) return it->second;
    std::vector<uint8_t> v(kBytes);
    for (uint64_t i = 0; i < kBytes; ++i)
      v[i] = static_cast<uint8_t>(i * 31 + seed);
    return patterns.emplace(seed, std::move(v)).first->second;
  }
  void fill(uint8_t seed) const {
    dev->copy(ptr, pattern(seed).data(), kBytes);
  }
  bool verify(uint8_t seed) const { return first_mismatch(seed) < 0; }

  /* What went wrong, in enough detail to tell the two candidate faults
   * apart: bytes never written keep the pattern that was there before, while
   * bytes written wrongly hold something else entirely. */
  struct Damage {
    int64_t first = -1;
    int64_t last = -1;
    uint64_t count = 0;
    uint8_t got = 0;
    uint8_t want = 0;
    bool looks_like_stale = false; /* still the other pattern */
    /* The same bytes, read a second time without touching anything. This is
     * what separates the two explanations: if they are right now, nothing
     * was lost and the copy had simply not landed when it reported that it
     * had; if they are still wrong, the data never arrived at all. */
    uint64_t count_on_reread = 0;
    uint64_t count_after_sync = 0;
  };
  Damage inspect(uint8_t seed, uint8_t previous) const {
    Damage d;
    scratch.resize(kBytes);
    dev->copy(scratch.data(), ptr, kBytes);
    auto const& want = pattern(seed);
    auto const& before = pattern(previous);
    uint64_t stale = 0;
    for (uint64_t i = 0; i < kBytes; ++i) {
      if (scratch[i] == want[i]) continue;
      if (d.first < 0) {
        d.first = static_cast<int64_t>(i);
        d.got = scratch[i];
        d.want = want[i];
      }
      d.last = static_cast<int64_t>(i);
      ++d.count;
      if (scratch[i] == before[i]) ++stale;
    }
    d.looks_like_stale = d.count > 0 && stale == d.count;
    if (d.count == 0) return d;

    reread.resize(kBytes);
    dev->copy(reread.data(), ptr, kBytes);
    for (uint64_t i = 0; i < kBytes; ++i)
      if (reread[i] != want[i]) ++d.count_on_reread;

    /* And once more behind a full device barrier, which settles whether any
     * work was still outstanding. */
    device_barrier_always();
    dev->copy(reread.data(), ptr, kBytes);
    for (uint64_t i = 0; i < kBytes; ++i)
      if (reread[i] != want[i]) ++d.count_after_sync;
    return d;
  }
  /* Where the first wrong byte is, or -1. A soak run reporting only that
   * something did not match says nothing about whether a transfer went
   * astray or a whole round never ran. */
  int64_t first_mismatch(uint8_t seed) const {
    scratch.resize(kBytes);
    dev->copy(scratch.data(), ptr, kBytes);
    auto const& want = pattern(seed);
    /* memcmp first: the byte-by-byte search is three times slower, and on a
     * soak run that difference is the run. Only a round that already failed
     * pays for finding out where. */
    if (std::memcmp(scratch.data(), want.data(), kBytes) == 0) return -1;
    for (uint64_t i = 0; i < kBytes; ++i)
      if (scratch[i] != want[i]) return static_cast<int64_t>(i);
    return -1;
  }

  mutable std::map<uint8_t, std::vector<uint8_t>> patterns;
  mutable std::vector<uint8_t> scratch;
  mutable std::vector<uint8_t> reread;
};

bool make_buffer(int gpu, Buffer* out) {
  int const index = gpu < 0 ? 0 : gpu;
  std::shared_ptr<DeviceBackend> dev;
  void* p = nullptr;
#if defined(HUX_LOOPBACK_CUDA)
  if (CudaBackend::create(index, &dev) != Status::kOk) return false;
  if (cudaSetDevice(index) != cudaSuccess ||
      cudaMalloc(&p, kBytes) != cudaSuccess) {
    std::printf("device allocation failed\n");
    return false;
  }
#elif defined(HUX_LOOPBACK_ROCM)
  if (RocmBackend::create(index, &dev) != Status::kOk) return false;
  if (hipSetDevice(index) != hipSuccess ||
      hipMalloc(&p, kBytes) != hipSuccess) {
    std::printf("device allocation failed\n");
    return false;
  }
#elif defined(HUX_LOOPBACK_NEUWARE)
  if (NeuwareBackend::create(index, &dev) != Status::kOk) return false;
  if (cnrtSetDevice(index) != cnrtSuccess ||
      cnrtMalloc(&p, kBytes) != cnrtSuccess) {
    std::printf("device allocation failed\n");
    return false;
  }
#else
  (void)index;
  std::printf(
      "built without a device backend, and host memory cannot be exported to "
      "another process\n");
  return false;
#endif
  if (dev == nullptr) {
    std::printf("no device %d\n", gpu);
    return false;
  }
  /* Asked, not assumed: a backend that cannot export an allocation fails
   * later at registration, which says less about why. */
  if (!dev->caps().supports_ipc) {
    std::printf("device reports no IPC support\n");
    return false;
  }
  out->dev = dev;
  out->ptr = p;
  return true;
}

int run_server(int gpu) {
  Buffer buf;
  if (!make_buffer(gpu, &buf)) return 1;
  buf.fill(0x11);

  std::shared_ptr<IpcProvider> prov;
  if (IpcProvider::create(IpcConfig{}, buf.dev, &prov) != Status::kOk) {
    std::printf("[server] provider create failed\n");
    return 1;
  }

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, buf.dev, prov, &engine) != Status::kOk) {
    std::printf("[server] engine create failed\n");
    return 1;
  }

  MemoryRegionPtr region;
  if (engine->register_memory(
          buf.ptr, kBytes, AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
          &region) != Status::kOk) {
    std::printf("[server] register failed -- this memory cannot be exported\n");
    return 1;
  }
  std::vector<uint8_t> desc, meta;
  region->export_descriptor(&desc);
  /* The engine's, not the provider's: it carries the identity a peer reads
   * to decide this path can reach us at all. */
  engine->local_metadata(&meta);

  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(g_port);
  /* Checked, because an unchecked bind is how a second server silently hands
   * its clients to the first one still holding the port -- and the run that
   * follows measures the wrong process. */
  if (::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
      ::listen(srv, 1) != 0) {
    std::printf("[server] port %u is already taken\n", g_port);
    return 1;
  }
  std::printf("[server] waiting on :%u\n", g_port);
  int fd = ::accept(srv, nullptr, nullptr);
  /* Without this the small request-and-reply of a soak round meets Nagle on
   * one side and the delayed acknowledgement on the other, and every round
   * costs about 40 ms per direction -- which reads as a slow transfer and is
   * nothing of the kind. TCP_NODELAY is not inherited from the listening
   * socket, so it is set on the accepted one. */
  int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  send_blob(fd, meta.data(), static_cast<uint32_t>(meta.size()));
  send_blob(fd, desc.data(), static_cast<uint32_t>(desc.size()));

  ProviderConnectionPtr conn;
  if (prov->accept(20000, &conn) != Status::kOk) {
    std::printf("[server] ipc accept failed\n");
    return 1;
  }
  std::printf("[server] %s\n", prov->describe().c_str());

  /* The client drives; this side answers until it says it is finished. Each
   * round it verifies what was written, so a corruption at hour three is
   * caught at hour three rather than at the end. */
  std::vector<uint8_t> ack;
  uint64_t rounds = 0, bad = 0, handoffs = 0;
  for (;;) {
    if (!recv_blob(fd, &ack)) break;
    if (!ack.empty() && ack[0] == 'k') break;
    ++rounds;
    if (!buf.verify(0x22)) ++bad;
    buf.fill(0x11);
    device_barrier();
    /* Drained every round. Progress is explicit here, and each write the peer
     * makes leaves a ready handoff on the control channel: left unread it
     * fills the socket, and the peer's sends start blocking on a buffer this
     * side never empties -- which looks like a slow transfer and is not one. */
    std::vector<ReadyEventPtr> evs;
    engine->poll_ready_events(32, &evs);
    handoffs += evs.size();
    std::vector<RequestPtr> fin;
    engine->poll_completions(32, &fin);
    send_blob(fd, "r", 1);
  }
  if (rounds > 0) {
    std::printf("[server] soak rounds: %llu, mismatches: %llu\n",
                (unsigned long long)rounds, (unsigned long long)bad);
  }

  std::vector<ReadyEventPtr> ready;
  for (int i = 0; i < 2000 && ready.empty() && handoffs == 0; ++i) {
    engine->poll_ready_events(8, &ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::printf("[server] ready handoffs received: %llu\n",
              (unsigned long long)(ready.size() + handoffs));
  /* After a soak run the last thing this side did was refill for the peer's
   * read, so that is what should be there. */
  std::printf("[server] verifying what the client wrote: %s\n",
              buf.verify(rounds > 0 ? 0x11 : 0x22) ? "OK" : "MISMATCH");

  /* The point of the exercise: the peer still holds a mapping of this
   * allocation, and releasing the region has to wait for it to let go before
   * the memory could be freed. */
  /* The client waits a moment before it processes the withdrawal, so a
   * release that did not really wait would come back far too quickly. */
  auto const t0 = std::chrono::steady_clock::now();
  Status rs = engine->deregister_memory(region);
  /* The handle goes, and then the cache is asked to let go too: without that
   * the registration is kept for the next caller and the peer is never asked
   * to unmap, so this memory could not safely be freed. */
  region.reset();
  uint32_t released = 0;
  engine->release_cached_registrations(&released);
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  std::printf(
      "[server] release: %s after %lld ms (peer confirmed it unmapped)\n",
      to_string(rs), static_cast<long long>(ms));
  std::printf("[server] %s\n", prov->describe().c_str());

  send_blob(fd, "z", 1);
  ::close(fd);
  ::close(srv);
  return rs == Status::kOk ? 0 : 1;
}

int run_client(int gpu, uint64_t loops) {
  Buffer buf;
  if (!make_buffer(gpu, &buf)) return 1;
  buf.fill(0x99); /* overwritten by the read, so a stale buffer cannot pass */

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(g_port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool linked = false;
  for (int i = 0; i < 20 && !linked; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0)
      linked = true;
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (!linked) {
    std::printf("[client] no server answering on :%u\n", g_port);
    return 1;
  }
  int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  std::vector<uint8_t> meta, desc;
  if (!recv_blob(fd, &meta) || !recv_blob(fd, &desc) || meta.empty()) {
    std::printf("[client] server closed before its handshake\n");
    return 1;
  }

  std::shared_ptr<IpcProvider> prov;
  if (IpcProvider::create(IpcConfig{}, buf.dev, &prov) != Status::kOk) {
    std::printf("[client] provider create failed\n");
    return 1;
  }

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, buf.dev, prov, &engine) != Status::kOk) return 1;

  MemoryRegionPtr local;
  if (engine->register_memory(
          buf.ptr, kBytes, AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
          &local) != Status::kOk)
    return 1;

  PeerPtr peer;
  if (engine->add_peer(meta, &peer) != Status::kOk) {
    std::printf("[client] ipc connect failed\n");
    return 1;
  }
  std::printf("[client] %s\n", prov->describe().c_str());

  RemoteRegionPtr remote;
  if (peer->import_region(desc, &remote) != Status::kOk) {
    std::printf("[client] import_region failed\n");
    return 1;
  }

  /* Progress is explicit, so completions are collected here rather than by a
   * thread the test does not own. */
  auto drive = [&](RequestPtr const& req) {
    std::vector<RequestPtr> done;
    for (int i = 0; i < 200000; ++i) {
      engine->poll_completions(32, &done);
      bool fin = false;
      req->test(&fin);
      if (fin) return req->wait(5000);
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return Status::kTimeout;
  };

  RegionView lv, rv;
  local->view(0, kBytes, &lv);
  remote->view(0, kBytes, &rv);

  std::printf("\n=== read: peer process -> this process ===\n");
  RequestPtr rd;
  Status s = engine->read(peer.get(), lv, rv, {}, &rd);
  if (s != Status::kOk) {
    std::printf("   submit failed: %s\n", to_string(s));
    return 1;
  }
  s = drive(rd);
  std::printf("   status %s, reached target_ready=%d\n", to_string(s),
              rd->reached(Stage::kTargetReady));
  std::printf("   data verified: %s\n",
              s == Status::kOk && buf.verify(0x11) ? "OK" : "FAILED");

  std::printf("\n=== write: this process -> peer process ===\n");
  buf.fill(0x22);
  RequestPtr wr;
  s = engine->write(peer.get(), lv, rv, {}, &wr);
  if (s != Status::kOk) {
    std::printf("   submit failed: %s\n", to_string(s));
    return 1;
  }
  s = drive(wr);
  std::printf("   status %s\n", to_string(s));

  auto const st = prov->stats();
  std::printf("\n=== what the path cost ===\n");
  std::printf("   requested %llu B, copied %llu B\n",
              (unsigned long long)st.payload_bytes,
              (unsigned long long)st.payload_bytes_copied);
  std::printf("   -> a mapping removes the network, not the copy\n");
  std::printf("   sub-operations: posted=%llu completed=%llu failed=%llu\n",
              (unsigned long long)st.subops_posted,
              (unsigned long long)st.subops_completed,
              (unsigned long long)st.subops_failed);
  std::printf("   %s\n", prov->describe().c_str());

  /* A soak run repeats the pair of transfers; every round verifies both
   * directions, so a fault that only appears after hours is still caught
   * where it happens. */
  if (loops > 0) {
    auto const start = std::chrono::steady_clock::now();
    /* Split by phase, because a soak run that is slower than expected should
     * say which part is slow rather than leave it to be guessed. */
    int64_t us_read = 0, us_verify = 0, us_write = 0, us_peer = 0;
    /* Counted apart. A submission refused, a request that never finished and
     * bytes that arrived wrong are three different faults, and a single
     * total cannot tell them apart. */
    uint64_t bad_write = 0, bad_read = 0, bad_bytes = 0;
    bool told = false;
    auto const report = [&](char const* what, uint64_t round, Status st,
                            int64_t at) {
      if (told) return;
      told = true;
      std::printf("   FIRST FAULT at round %llu: %s, status %s",
                  (unsigned long long)round, what, to_string(st));
      if (at >= 0) std::printf(", first wrong byte at %lld", (long long)at);
      std::printf("\n");
      std::fflush(stdout);
    };
    auto const tick = [] { return std::chrono::steady_clock::now(); };
    auto const since = [](std::chrono::steady_clock::time_point t) {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - t)
          .count();
    };
    /* Write first, then let the peer check and refill, then read back what it
     * put there. Reading first would check bytes this side wrote a moment
     * ago, which proves nothing about the round. */
    for (uint64_t i = 0; i < loops; ++i) {
      RequestPtr r1, r2;
      auto t = tick();
      buf.fill(0x22);
      us_verify += since(t);
      t = tick();
      Status ws = engine->write(peer.get(), lv, rv, {}, &r2);
      if (ws == Status::kOk) ws = drive(r2);
      if (ws != Status::kOk) {
        ++bad_write;
        report("write", i, ws, -1);
      }
      us_write += since(t);

      t = tick();
      send_blob(fd, "n", 1);
      std::vector<uint8_t> reply;
      if (!recv_blob(fd, &reply)) {
        std::printf("   peer stopped answering at round %llu\n",
                    (unsigned long long)i);
        break;
      }
      us_peer += since(t);

      /* Only now: the peer has verified what was written and put the read
       * pattern back. */
      t = tick();
      Status rs = engine->read(peer.get(), lv, rv, {}, &r1);
      if (rs == Status::kOk) rs = drive(r1);
      if (rs != Status::kOk) {
        ++bad_read;
        report("read", i, rs, -1);
      }
      us_read += since(t);
      t = tick();
      int64_t const at = buf.first_mismatch(0x11);
      if (at >= 0) {
        ++bad_bytes;
        report("bytes", i, Status::kOk, at);
        if (bad_bytes <= 3) {
          auto const d = buf.inspect(0x11, 0x22);
          std::printf(
              "     damage: %llu bytes in [%lld,%lld], got 0x%02x want 0x%02x,"
              " %s\n",
              (unsigned long long)d.count, (long long)d.first,
              (long long)d.last, d.got, d.want,
              d.looks_like_stale ? "all of it is what this side wrote before "
                                   "-- the read never covered it"
                                 : "not the previous contents -- something "
                                   "else landed there");
          std::printf(
              "     re-read immediately: %llu still wrong;"
              " after a device barrier: %llu still wrong  -> %s\n",
              (unsigned long long)d.count_on_reread,
              (unsigned long long)d.count_after_sync,
              d.count_after_sync == 0
                  ? "the bytes arrive late; nothing was lost"
                  : "the bytes never arrive; the copy really is short");
          std::fflush(stdout);
        }
      }
      us_verify += since(t);

      if ((i + 1) % 10000 == 0) {
        auto const secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        std::printf(
            "   %llu rounds, %llds elapsed, faults: write %llu read %llu "
            "bytes %llu (read %lldus verify %lldus write %lldus peer %lldus "
            "each)\n",
            (unsigned long long)(i + 1), (long long)secs,
            (unsigned long long)bad_write, (unsigned long long)bad_read,
            (unsigned long long)bad_bytes,
            (long long)(us_read / (int64_t)(i + 1)),
            (long long)(us_verify / (int64_t)(i + 1)),
            (long long)(us_write / (int64_t)(i + 1)),
            (long long)(us_peer / (int64_t)(i + 1)));
        std::fflush(stdout);
      }
    }
    std::printf(
        "   soak finished: %llu rounds, faults: write %llu read %llu "
        "bytes %llu\n",
        (unsigned long long)loops, (unsigned long long)bad_write,
        (unsigned long long)bad_read, (unsigned long long)bad_bytes);
  }

  send_blob(fd, "k", 1);

  /* Deliberately idle: the peer is dropping the region during this pause, and
   * its release has to wait for the mapping here to go rather than assume it
   * has. */
  std::printf("\n=== the peer drops the region while this side holds it ===\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::printf("   held for 300 ms before polling, mappings: %s\n",
              prov->describe().c_str());
  std::vector<ControlMessage> msgs;
  for (int i = 0; i < 50; ++i) {
    prov->poll_control(8, &msgs);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::printf("   after polling: %s\n", prov->describe().c_str());

  std::vector<uint8_t> done;
  recv_blob(fd, &done);
  ::close(fd);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: %s server|client [--gpu N] [--loop ROUNDS] [--port P]"
        " [--sync]\n",
        argv[0]);
    return 2;
  }
  int gpu = 0;
  uint64_t loops = 0;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--sync") == 0) g_sync_after_fill = true;
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], "--gpu") == 0) gpu = std::atoi(argv[i + 1]);
    /* Rounds of read-and-verify plus write-and-verify, for a soak run. */
    if (std::strcmp(argv[i], "--loop") == 0)
      loops = std::strtoull(argv[i + 1], nullptr, 10);
    if (std::strcmp(argv[i], "--port") == 0)
      g_port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
  }
  if (std::strcmp(argv[1], "server") == 0) return run_server(gpu);
  return run_client(gpu, loops);
}
