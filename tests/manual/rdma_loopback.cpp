/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * End-to-end check over real RDMA: application-owned memory on one side is
 * read from, and written to, memory owned by the application on the other,
 * and the bytes are verified. Two processes; no library-owned buffer is
 * involved anywhere.
 *
 *   ./rdma_loopback server [--gpu N]
 *   ./rdma_loopback client <ip> [--gpu N]
 *
 * Without --gpu both sides use host memory, which is what runs anywhere. With
 * it, memory comes from the GPU, which needs a card that supports GDR. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "core/region_impl.h"
#include "hux/engine.h"
#include "transport/rdma/rdma_provider.h"

#ifdef HUX_LOOPBACK_CUDA
#include "device/cuda_backend.h"
#include <cuda_runtime.h>
#endif

using namespace hux;

namespace {

constexpr uint16_t kMetaPort = 18515;
constexpr uint64_t kBytes = 1u << 20;

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

struct Buffer {
  void* ptr = nullptr;
  bool on_gpu = false;

  void fill(uint8_t seed) {
    std::vector<uint8_t> host(kBytes);
    for (uint64_t i = 0; i < kBytes; ++i)
      host[i] = static_cast<uint8_t>(i * 31 + seed);
    store(host.data());
  }
  void store(void const* src) {
#ifdef HUX_LOOPBACK_CUDA
    if (on_gpu) {
      cudaMemcpy(ptr, src, kBytes, cudaMemcpyHostToDevice);
      return;
    }
#endif
    std::memcpy(ptr, src, kBytes);
  }
  void load(void* dst) const {
#ifdef HUX_LOOPBACK_CUDA
    if (on_gpu) {
      cudaMemcpy(dst, ptr, kBytes, cudaMemcpyDeviceToHost);
      return;
    }
#endif
    std::memcpy(dst, ptr, kBytes);
  }
};

Buffer make_buffer(int gpu) {
  Buffer b;
#ifdef HUX_LOOPBACK_CUDA
  if (gpu >= 0) {
    cudaSetDevice(gpu);
    if (cudaMalloc(&b.ptr, kBytes) != cudaSuccess) {
      std::printf("cudaMalloc failed\n");
      std::exit(1);
    }
    b.on_gpu = true;
    return b;
  }
#else
  (void)gpu;
#endif
  b.ptr = std::aligned_alloc(4096, kBytes);
  return b;
}

bool verify(Buffer const& b, uint8_t seed) {
  std::vector<uint8_t> got(kBytes);
  b.load(got.data());
  for (uint64_t i = 0; i < kBytes; ++i) {
    uint8_t want = static_cast<uint8_t>(i * 31 + seed);
    if (got[i] != want) {
      std::printf("   mismatch at %llu: got %u want %u\n",
                  (unsigned long long)i, got[i], want);
      return false;
    }
  }
  return true;
}

int run_server(int gpu) {
  RdmaConfig cfg;
  cfg.advertise_ip = "0.0.0.0";
  std::shared_ptr<RdmaProvider> prov;
  if (RdmaProvider::create(cfg, &prov) != Status::kOk) {
    std::printf("provider create failed\n");
    return 1;
  }

  Buffer buf = make_buffer(gpu);
  buf.fill(0x11);  /* the client will read this, and later overwrite it */

  uint64_t lkey = 0, rkey = 0;
  if (prov->register_region(buf.ptr, kBytes, DeviceId{},
                            AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
                            &lkey, &rkey) != Status::kOk) {
    std::printf("register failed\n");
    return 1;
  }

  RegionDescriptor d;
  d.region = 1;
  d.generation = 1;
  d.base = reinterpret_cast<uint64_t>(buf.ptr);
  d.length = kBytes;
  d.remote_key = rkey;
  d.access = AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite;
  std::vector<uint8_t> desc;
  encode_descriptor(d, &desc);

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
  std::printf("[server] waiting on :%u (%s memory)\n", kMetaPort,
              buf.on_gpu ? "device" : "host");
  int fd = ::accept(srv, nullptr, nullptr);

  send_blob(fd, meta.data(), static_cast<uint32_t>(meta.size()));
  send_blob(fd, desc.data(), static_cast<uint32_t>(desc.size()));

  /* An engine over the same provider, so the arrival of a peer's write can be
   * collected as a ReadyEvent -- the handoff a target owner needs before
   * scheduling anything that consumes the data. */
  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, nullptr, prov, &engine) != Status::kOk) {
    std::printf("[server] engine create failed\n");
    return 1;
  }

  ProviderConnectionPtr conn;
  if (prov->accept(20000, &conn) != Status::kOk) {
    std::printf("[server] rdma accept failed\n");
    return 1;
  }
  std::printf("[server] connected, holding memory\n");

  std::vector<uint8_t> ack;
  recv_blob(fd, &ack);  /* client says it is done reading and writing */

  std::vector<ReadyEventPtr> ready;
  for (int i = 0; i < 2000 && ready.empty(); ++i) {
    engine->poll_ready_events(8, &ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::printf("[server] ready handoffs received: %zu\n", ready.size());
  if (!ready.empty()) {
    std::printf("[server]   first names request %llu from peer %llu\n",
                (unsigned long long)ready[0]->request(),
                (unsigned long long)ready[0]->peer());
  }

  std::printf("[server] verifying what the client wrote: %s\n",
              verify(buf, 0x22) ? "OK" : "MISMATCH");
  send_blob(fd, "z", 1);
  ::close(fd);
  ::close(srv);
  return 0;
}

int run_client(std::string const& ip, int gpu) {
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

  /* The metadata advertises 0.0.0.0 from the server's side; dial the address
   * we actually reached it on. */
  uint16_t port = meta[0] | (uint16_t(meta[1]) << 8);
  std::vector<uint8_t> fixed;
  fixed.push_back(meta[0]);
  fixed.push_back(meta[1]);
  fixed.push_back(meta[2]);
  fixed.push_back(meta[3]);
  fixed.push_back(static_cast<uint8_t>(ip.size() & 0xff));
  fixed.push_back(static_cast<uint8_t>(ip.size() >> 8));
  fixed.insert(fixed.end(), ip.begin(), ip.end());
  (void)port;

  RdmaConfig cfg;
  cfg.advertise_ip = ip;
  std::shared_ptr<RdmaProvider> prov;
  if (RdmaProvider::create(cfg, &prov) != Status::kOk) {
    std::printf("provider create failed\n");
    return 1;
  }

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  ecfg.chunk_bytes = 256u << 10;  /* forces several sub-operations */
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, nullptr, prov, &engine) != Status::kOk) {
    std::printf("engine create failed\n");
    return 1;
  }

  Buffer buf = make_buffer(gpu);
  buf.fill(0x00);

  MemoryRegionPtr local;
  if (engine->register_memory(buf.ptr, kBytes,
                              AccessFlags::kLocalRead | AccessFlags::kLocalWrite,
                              &local) != Status::kOk) {
    std::printf("register failed\n");
    return 1;
  }

  PeerPtr peer;
  if (engine->add_peer(fixed, &peer) != Status::kOk) {
    std::printf("add_peer failed\n");
    return 1;
  }
  RemoteRegionPtr remote;
  if (peer->import_region(desc, &remote) != Status::kOk) {
    std::printf("import_region failed\n");
    return 1;
  }
  std::printf("[client] connected (%s memory)\n", buf.on_gpu ? "device" : "host");

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

  std::printf("\n=== read: server memory -> client memory ===\n");
  RequestPtr rd;
  Status s = engine->read(peer.get(), lv, rv, {}, &rd);
  if (s != Status::kOk) {
    std::printf("   submit failed: %s\n", to_string(s));
    return 1;
  }
  s = drive(rd);
  std::printf("   status %s, reached target_ready=%d\n", to_string(s),
              rd->reached(Stage::kTargetReady));
  bool read_ok = s == Status::kOk && verify(buf, 0x11);
  std::printf("   data verified: %s\n", read_ok ? "OK" : "FAILED");

  std::printf("\n=== write: client memory -> server memory ===\n");
  buf.fill(0x22);
  RequestPtr wr;
  s = engine->write(peer.get(), lv, rv, {}, &wr);
  if (s != Status::kOk) {
    std::printf("   submit failed: %s\n", to_string(s));
    return 1;
  }
  s = drive(wr);
  std::printf("   status %s\n", to_string(s));

  /* The same situation that leaves UCCL's poll_async waiting forever: a read
   * against an address the peer never exported. The RDMA layer raises a
   * remote access error; what matters is whether the caller learns of it and
   * whether the request reaches a terminal state so its memory can be freed. */
  std::printf("\n=== failure is reported, not hung ===\n");
  {
    RegionDescriptor bogus;
    bogus.region = 99;
    bogus.generation = 1;
    bogus.base = 0xdead0000ull;   /* never registered by the peer */
    bogus.length = kBytes;
    bogus.remote_key = 0x12345678u;
    bogus.access = AccessFlags::kRemoteRead;
    std::vector<uint8_t> bd;
    encode_descriptor(bogus, &bd);

    RemoteRegionPtr bad_remote;
    if (peer->import_region(bd, &bad_remote) == Status::kOk) {
      RegionView brv;
      bad_remote->view(0, kBytes, &brv);
      RequestPtr bad;
      Status bs = engine->read(peer.get(), lv, brv, {}, &bad);
      if (bs == Status::kOk) {
        auto t0 = std::chrono::steady_clock::now();
        Status ds = drive(bad);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        bool terminal = is_terminal(bad->state());
        std::printf("   resolved in %lld ms: status=%s state=%s terminal=%d\n",
                    (long long)ms, to_string(ds), to_string(bad->state()),
                    terminal);
        std::printf("   error: %s  may_have_modified_target=%d\n",
                    to_string(bad->error().status),
                    bad->error().may_have_modified_target);
        std::printf("   reached failed_safe=%d target_ready=%d\n",
                    bad->reached(Stage::kFailedSafe),
                    bad->reached(Stage::kTargetReady));
        std::printf("   -> %s\n",
                    terminal ? "terminal, so its resources can be released"
                             : "NOT terminal: resources cannot be freed");
      } else {
        std::printf("   submit refused up front: %s\n", to_string(bs));
      }
    }
  }

  /* The zero-copy claim, as a number rather than an assertion. A transfer in
   * place leaves payload_bytes_copied at zero; a path that staged through an
   * intermediate buffer would report what it moved. */
  std::printf("\n=== payload copies on the whole path ===\n");
  {
    EngineStats st = engine->stats();
    std::printf("   requested %llu B across %llu sub-operations\n",
                (unsigned long long)st.payload_bytes,
                (unsigned long long)st.subops_posted);
    std::printf("   extra payload copied: %llu B\n",
                (unsigned long long)st.payload_bytes_copied);
    std::printf("   -> %s\n", st.payload_bytes_copied == 0
                                  ? "zero-copy: the NIC used the caller's memory"
                                  : "NOT zero-copy");
    std::printf("   requests: accepted=%llu succeeded=%llu failed=%llu\n",
                (unsigned long long)st.requests_accepted,
                (unsigned long long)st.requests_succeeded,
                (unsigned long long)st.requests_failed);
  }

  std::printf("\n=== repeated queries agree ===\n");
  bool d1 = false, d2 = false;
  wr->test(&d1);
  wr->test(&d2);
  std::printf("   test twice: %d %d   wait again: %s\n", d1, d2,
              to_string(wr->wait(100)));

  send_blob(fd, "x", 1);
  std::vector<uint8_t> fin;
  recv_blob(fd, &fin);
  ::close(fd);
  return read_ok && s == Status::kOk ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s server|client <ip> [--gpu N]\n", argv[0]);
    return 2;
  }
  int gpu = -1;
  for (int i = 1; i < argc - 1; ++i)
    if (std::strcmp(argv[i], "--gpu") == 0) gpu = std::atoi(argv[i + 1]);

  if (std::strcmp(argv[1], "server") == 0) return run_server(gpu);
  if (argc < 3) {
    std::printf("client needs an ip\n");
    return 2;
  }
  return run_client(argv[2], gpu);
}
