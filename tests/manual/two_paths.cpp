/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * One engine, two live transports at once.
 *
 * Three processes. The client holds both an IPC provider and an RDMA one and
 * talks to a peer in the next process on this host and a peer on another
 * machine at the same time, without knowing in advance which transport either
 * will take.
 *
 *   on the far machine:   ./hux_two_paths far  --gpu N --local <its ip>
 *   on this machine:      ./hux_two_paths near --gpu N
 *                         ./hux_two_paths client --gpu N --far <far ip>
 *                                                --local <this ip> */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "harness.h"
#include "hux/engine.h"
#include "transport/ipc/ipc_provider.h"
#include "transport/rdma/rdma_provider.h"

using namespace hux;
using namespace hux::manual;

namespace {

constexpr uint64_t kBytes = 1u << 20;
constexpr uint16_t kNearPort = 18520;
constexpr uint16_t kFarPort = 18521;

/* A side that waits to be reached. `ipc` decides which transport it offers,
 * which is how the client ends up choosing differently for the two of them
 * without being told to. */
int run_server(char const* role, int gpu, bool ipc, std::string const& local_ip,
               uint16_t port) {
  Buffer buf;
  if (!buf.make(gpu, kBytes)) {
    std::printf("[%s] no usable device\n", role);
    return 1;
  }
  buf.fill(0x11);

  std::vector<TransportProviderPtr> providers;
  std::shared_ptr<IpcProvider> ipc_prov;
  std::shared_ptr<RdmaProvider> rdma_prov;
  if (ipc) {
    if (IpcProvider::create(IpcConfig{}, buf.dev, &ipc_prov) != Status::kOk) {
      std::printf("[%s] ipc provider failed\n", role);
      return 1;
    }
    providers.push_back(ipc_prov);
  }
  RdmaConfig rcfg;
  rcfg.advertise_ip = local_ip;
  if (RdmaProvider::create(rcfg, &rdma_prov) != Status::kOk) {
    std::printf("[%s] rdma provider failed\n", role);
    return 1;
  }
  providers.push_back(rdma_prov);

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, buf.dev, providers, &engine) != Status::kOk) return 1;

  MemoryRegionPtr region;
  if (engine->register_memory(
          buf.ptr, kBytes, AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
          &region) != Status::kOk) {
    std::printf("[%s] register failed\n", role);
    return 1;
  }
  std::vector<uint8_t> desc, meta;
  region->export_descriptor(&desc);
  engine->local_metadata(&meta);

  int srv = listen_on(port);
  if (srv < 0) return 1;
  std::printf("[%s] waiting on :%u, offering %s\n", role, port,
              ipc ? "ipc and rdma" : "rdma only");
  std::fflush(stdout);
  int fd = ::accept(srv, nullptr, nullptr);
  nodelay(fd);
  send_blob(fd, meta.data(), static_cast<uint32_t>(meta.size()));
  send_blob(fd, desc.data(), static_cast<uint32_t>(desc.size()));

  /* Both transports wait at once. Which one the client dials is its
   * decision, made from the metadata just sent, and asking it first would
   * deadlock: it cannot answer until it has connected, and it cannot connect
   * until this side is accepting. */
  ProviderConnectionPtr ipc_conn, rdma_conn;
  std::thread ipc_wait;
  if (ipc_prov != nullptr)
    ipc_wait = std::thread([&] { ipc_prov->accept(25000, &ipc_conn); });
  std::thread rdma_wait([&] { rdma_prov->accept(25000, &rdma_conn); });

  std::vector<uint8_t> which;
  recv_blob(fd, &which);
  std::string const chosen(which.begin(), which.end());
  if (ipc_wait.joinable()) ipc_wait.join();
  rdma_wait.join();

  ProviderConnectionPtr conn = chosen == "ipc" ? ipc_conn : rdma_conn;
  if (conn == nullptr) {
    std::printf("[%s] the client said %s but nothing was accepted there\n",
                role, chosen.empty() ? "nothing" : chosen.c_str());
    return 1;
  }
  std::printf("[%s] accepted over %s\n", role, chosen.c_str());
  std::fflush(stdout);

  std::vector<uint8_t> done;
  recv_blob(fd, &done);
  std::printf("[%s] verifying what the client wrote: %s\n", role,
              buf.verify(0x22) ? "OK" : "MISMATCH");
  send_blob(fd, "z", 1);
  ::close(fd);
  ::close(srv);
  return 0;
}

int run_client(int gpu, std::string const& far_ip,
               std::string const& local_ip) {
  Buffer buf;
  if (!buf.make(gpu, kBytes)) return 1;

  std::shared_ptr<IpcProvider> ipc_prov;
  std::shared_ptr<RdmaProvider> rdma_prov;
  if (IpcProvider::create(IpcConfig{}, buf.dev, &ipc_prov) != Status::kOk)
    return 1;
  RdmaConfig rcfg;
  rcfg.advertise_ip = local_ip;
  if (RdmaProvider::create(rcfg, &rdma_prov) != Status::kOk) return 1;

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  std::vector<TransportProviderPtr> providers{ipc_prov, rdma_prov};
  if (make_engine(ecfg, buf.dev, providers, &engine) != Status::kOk) return 1;

  MemoryRegionPtr local;
  if (engine->register_memory(
          buf.ptr, kBytes, AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
          &local) != Status::kOk)
    return 1;
  /* One registration, and it carries a key for each transport -- which is
   * what lets the same memory serve both peers. */
  std::vector<uint8_t> own;
  local->export_descriptor(&own);
  std::printf("[client] one registration, descriptor is %zu bytes\n",
              own.size());

  auto drive = [&](RequestPtr const& req) {
    std::vector<RequestPtr> done;
    for (int i = 0; i < 400000; ++i) {
      engine->poll_completions(32, &done);
      bool fin = false;
      req->test(&fin);
      if (fin) return req->wait(5000);
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return Status::kTimeout;
  };

  struct Target {
    char const* name;
    std::string ip;
    uint16_t port;
  };
  Target targets[] = {{"near", "127.0.0.1", kNearPort},
                      {"far", far_ip, kFarPort}};

  int failures = 0;
  for (auto const& t : targets) {
    int fd = dial(t.ip, t.port, 20);
    if (fd < 0) {
      std::printf("[client] no %s server at %s:%u\n", t.name, t.ip.c_str(),
                  t.port);
      ++failures;
      continue;
    }
    std::vector<uint8_t> meta, desc;
    if (!recv_blob(fd, &meta) || !recv_blob(fd, &desc)) {
      std::printf("[client] %s closed before its handshake\n", t.name);
      ++failures;
      continue;
    }

    PeerPtr peer;
    Status s = engine->add_peer(meta, &peer);
    if (s != Status::kOk) {
      std::printf("[client] add_peer(%s) failed: %s\n", t.name, to_string(s));
      ++failures;
      continue;
    }
    std::printf("\n=== %s peer: place %s, path %s, provider %s ===\n", t.name,
                to_string(peer->caps().place), to_string(peer->caps().path),
                peer->caps().provider.c_str());
    std::fflush(stdout);
    send_blob(fd, peer->caps().provider.data(),
              static_cast<uint32_t>(peer->caps().provider.size()));

    RemoteRegionPtr remote;
    if (peer->import_region(desc, &remote) != Status::kOk) {
      std::printf("   import_region failed\n");
      ++failures;
      continue;
    }

    RegionView lv, rv;
    local->view(0, kBytes, &lv);
    remote->view(0, kBytes, &rv);

    RequestPtr rd;
    buf.fill(0x99);
    if (engine->read(peer.get(), lv, rv, {}, &rd) != Status::kOk ||
        drive(rd) != Status::kOk) {
      std::printf("   read failed\n");
      ++failures;
      continue;
    }
    bool const read_ok = buf.verify(0x11);
    std::printf("   read %llu B: %s\n", (unsigned long long)kBytes,
                read_ok ? "verified" : "MISMATCH");
    if (!read_ok) ++failures;

    buf.fill(0x22);
    RequestPtr wr;
    if (engine->write(peer.get(), lv, rv, {}, &wr) != Status::kOk ||
        drive(wr) != Status::kOk) {
      std::printf("   write failed\n");
      ++failures;
      continue;
    }
    std::printf("   write %llu B: submitted and completed\n",
                (unsigned long long)kBytes);
    send_blob(fd, "k", 1);
    std::vector<uint8_t> ack;
    recv_blob(fd, &ack);
    ::close(fd);
  }

  auto const st = engine->stats();
  std::printf("\n=== both paths, one engine ===\n");
  std::printf("   sub-operations: posted=%llu completed=%llu failed=%llu\n",
              (unsigned long long)st.subops_posted,
              (unsigned long long)st.subops_completed,
              (unsigned long long)st.subops_failed);
  std::printf("   payload %llu B, extra copies %llu B\n",
              (unsigned long long)st.payload_bytes,
              (unsigned long long)st.payload_bytes_copied);
  std::printf(
      "   -> the copies are the IPC half; the network half is in place\n");
  std::printf("   %s\n", engine->describe().c_str());
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s near|far|client [--gpu N] [--far IP] [--local IP]\n",
                argv[0]);
    return 2;
  }
  int gpu = 0;
  std::string far_ip = "127.0.0.1", local_ip = "127.0.0.1";
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], "--gpu") == 0) gpu = std::atoi(argv[i + 1]);
    if (std::strcmp(argv[i], "--far") == 0) far_ip = argv[i + 1];
    if (std::strcmp(argv[i], "--local") == 0) local_ip = argv[i + 1];
  }
  if (std::strcmp(argv[1], "near") == 0)
    return run_server("near", gpu, true, local_ip, kNearPort);
  if (std::strcmp(argv[1], "far") == 0)
    return run_server("far", gpu, false, local_ip, kFarPort);
  return run_client(gpu, far_ip, local_ip);
}
