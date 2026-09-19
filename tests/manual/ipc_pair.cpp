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
#include <cuda_runtime.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "device/cuda_backend.h"
#include "hux/engine.h"
#include "transport/ipc/ipc_provider.h"

using namespace hux;

namespace {

constexpr uint64_t kBytes = 4u << 20;
constexpr uint16_t kMetaPort = 18516;

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

  void fill(uint8_t seed) const {
    std::vector<uint8_t> host(kBytes);
    for (uint64_t i = 0; i < kBytes; ++i)
      host[i] = static_cast<uint8_t>(i * 31 + seed);
    dev->copy(ptr, host.data(), kBytes);
  }
  bool verify(uint8_t seed) const {
    std::vector<uint8_t> host(kBytes, 0);
    dev->copy(host.data(), ptr, kBytes);
    for (uint64_t i = 0; i < kBytes; ++i)
      if (host[i] != static_cast<uint8_t>(i * 31 + seed)) return false;
    return true;
  }
};

bool make_buffer(int gpu, Buffer* out) {
  std::shared_ptr<DeviceBackend> dev;
  if (CudaBackend::create(gpu < 0 ? 0 : gpu, &dev) != Status::kOk) {
    std::printf("no CUDA device %d\n", gpu);
    return false;
  }
  if (!dev->caps().supports_ipc) {
    std::printf("device reports no IPC support\n");
    return false;
  }
  void* p = nullptr;
  if (cudaSetDevice(gpu < 0 ? 0 : gpu) != cudaSuccess ||
      cudaMalloc(&p, kBytes) != cudaSuccess) {
    std::printf("cudaMalloc failed\n");
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
  a.sin_port = htons(kMetaPort);
  ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  ::listen(srv, 1);
  std::printf("[server] waiting on :%u\n", kMetaPort);
  int fd = ::accept(srv, nullptr, nullptr);
  send_blob(fd, meta.data(), static_cast<uint32_t>(meta.size()));
  send_blob(fd, desc.data(), static_cast<uint32_t>(desc.size()));

  ProviderConnectionPtr conn;
  if (prov->accept(20000, &conn) != Status::kOk) {
    std::printf("[server] ipc accept failed\n");
    return 1;
  }
  std::printf("[server] %s\n", prov->describe().c_str());

  std::vector<uint8_t> ack;
  recv_blob(fd, &ack);

  std::vector<ReadyEventPtr> ready;
  for (int i = 0; i < 2000 && ready.empty(); ++i) {
    engine->poll_ready_events(8, &ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::printf("[server] ready handoffs received: %zu\n", ready.size());
  std::printf("[server] verifying what the client wrote: %s\n",
              buf.verify(0x22) ? "OK" : "MISMATCH");

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

int run_client(int gpu) {
  Buffer buf;
  if (!make_buffer(gpu, &buf)) return 1;
  buf.fill(0x99); /* overwritten by the read, so a stale buffer cannot pass */

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kMetaPort);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool linked = false;
  for (int i = 0; i < 20 && !linked; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0)
      linked = true;
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (!linked) {
    std::printf("[client] no server answering on :%u\n", kMetaPort);
    return 1;
  }

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
    std::printf("usage: %s server|client [--gpu N]\n", argv[0]);
    return 2;
  }
  int gpu = 0;
  for (int i = 1; i < argc - 1; ++i)
    if (std::strcmp(argv[i], "--gpu") == 0) gpu = std::atoi(argv[i + 1]);
  if (std::strcmp(argv[1], "server") == 0) return run_server(gpu);
  return run_client(gpu);
}
