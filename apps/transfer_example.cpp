/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * A complete transfer, start to finish, using only the public API.
 *
 * It is deliberately written the way an application would be: memory the
 * program already owns, one registration for the whole pool, transfers
 * described as views into it, and errors handled where they happen rather
 * than collected at the end.
 *
 *   ./transfer_example server
 *   ./transfer_example client <ip>
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "transport/rdma/rdma_provider.h"

using namespace hux;

namespace {

constexpr uint16_t kBootstrapPort = 18517;
constexpr uint64_t kPoolBytes = 4u << 20;

/* Descriptors and connection details have to reach the peer somehow. Any
 * channel does; this uses a socket to keep the example self-contained. */
bool send_blob(int fd, void const* p, uint32_t n) {
  uint32_t len = htonl(n);
  if (::send(fd, &len, 4, 0) != 4) return false;
  return ::send(fd, p, n, 0) == static_cast<ssize_t>(n);
}

bool recv_blob(int fd, std::vector<uint8_t>* out) {
  uint32_t len = 0;
  if (::recv(fd, &len, 4, MSG_WAITALL) != 4) return false;
  out->assign(ntohl(len), 0);
  return ::recv(fd, out->data(), out->size(), MSG_WAITALL) ==
         static_cast<ssize_t>(out->size());
}

int fail(char const* what, Status s) {
  std::fprintf(stderr, "%s: %s\n", what, to_string(s));
  return 1;
}

int run_server(char const* bind_ip) {
  RdmaConfig rc;
  rc.advertise_ip = bind_ip;
  std::shared_ptr<RdmaProvider> provider;
  Status s = RdmaProvider::create(rc, &provider);
  if (s != Status::kOk) return fail("RdmaProvider::create", s);

  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  s = make_engine(cfg, nullptr, provider, &engine);
  if (s != Status::kOk) return fail("make_engine", s);

  /* Memory the application owns. It is registered once, as a whole, and the
   * transfer below moves a view of it -- registering per transfer would pay
   * the cost every time for no benefit. */
  std::vector<uint8_t> pool(kPoolBytes);
  for (size_t i = 0; i < pool.size(); ++i)
    pool[i] = static_cast<uint8_t>(i * 7 + 3);

  MemoryRegionPtr region;
  s = engine->register_memory(
      pool.data(), pool.size(),
      AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite, &region);
  if (s != Status::kOk) return fail("register_memory", s);

  std::vector<uint8_t> descriptor;
  s = region->export_descriptor(&descriptor);
  if (s != Status::kOk) return fail("export_descriptor", s);

  std::vector<uint8_t> metadata;
  s = provider->local_metadata(&metadata);
  if (s != Status::kOk) return fail("local_metadata", s);

  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(kBootstrapPort);
  ::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ::listen(srv, 1);
  std::printf("waiting for a client on :%u\n", kBootstrapPort);

  int fd = ::accept(srv, nullptr, nullptr);
  send_blob(fd, metadata.data(), static_cast<uint32_t>(metadata.size()));
  send_blob(fd, descriptor.data(), static_cast<uint32_t>(descriptor.size()));

  ProviderConnectionPtr conn;
  s = provider->accept(30000, &conn);
  if (s != Status::kOk) return fail("accept", s);
  std::printf("connected; memory stays registered until the client is done\n");

  /* The pool must outlive every transfer into it. Returning here, or letting
   * the vector go, would free memory the peer's NIC is still writing to. */
  std::vector<uint8_t> ack;
  recv_blob(fd, &ack);

  std::vector<ReadyEventPtr> ready;
  engine->poll_ready_events(8, &ready);
  for (auto const& r : ready) {
    std::printf("peer wrote region %llu, bytes [%llu,%llu)\n",
                (unsigned long long)r->region(),
                (unsigned long long)r->span().offset,
                (unsigned long long)(r->span().offset + r->span().length));
  }

  ::close(fd);
  ::close(srv);
  return 0;
}

int run_client(std::string const& ip) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kBootstrapPort);
  ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
  for (int i = 0; i < 30; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
      break;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::vector<uint8_t> metadata, descriptor;
  recv_blob(fd, &metadata);
  recv_blob(fd, &descriptor);

  /* The server advertised the address it binds; dial the one it answered on. */
  std::vector<uint8_t> dial(metadata.begin(), metadata.begin() + 4);
  dial.push_back(static_cast<uint8_t>(ip.size() & 0xff));
  dial.push_back(static_cast<uint8_t>(ip.size() >> 8));
  dial.insert(dial.end(), ip.begin(), ip.end());

  RdmaConfig rc;
  rc.advertise_ip = ip;
  std::shared_ptr<RdmaProvider> provider;
  Status s = RdmaProvider::create(rc, &provider);
  if (s != Status::kOk) return fail("RdmaProvider::create", s);

  EngineConfig cfg;
  cfg.progress = ProgressMode::kExplicit;
  std::unique_ptr<Engine> engine;
  s = make_engine(cfg, nullptr, provider, &engine);
  if (s != Status::kOk) return fail("make_engine", s);

  std::vector<uint8_t> pool(kPoolBytes, 0);
  MemoryRegionPtr local;
  s = engine->register_memory(
      pool.data(), pool.size(),
      AccessFlags::kLocalRead | AccessFlags::kLocalWrite, &local);
  if (s != Status::kOk) return fail("register_memory", s);

  PeerPtr peer;
  s = engine->add_peer(dial, &peer);
  if (s != Status::kOk) return fail("add_peer", s);
  std::printf("connected over %s\n", to_string(peer->caps().path));

  RemoteRegionPtr remote;
  s = peer->import_region(descriptor, &remote);
  if (s != Status::kOk) return fail("import_region", s);

  /* Views name the bytes to move. Both sides must agree on length; offsets
   * are independent. */
  RegionView local_view, remote_view;
  s = local->view(0, kPoolBytes, &local_view);
  if (s != Status::kOk) return fail("local view", s);
  s = remote->view(0, kPoolBytes, &remote_view);
  if (s != Status::kOk) return fail("remote view", s);

  RequestPtr request;
  s = engine->read(peer.get(), local_view, remote_view, {}, &request);
  if (s != Status::kOk) return fail("read", s);

  /* Explicit progress: this thread drives the engine. With
   * ProgressMode::kThread a background thread does it and the loop is just
   * request->wait(). */
  std::vector<RequestPtr> completed;
  while (!is_terminal(request->state()))
    engine->poll_completions(32, &completed);

  s = request->wait(0);
  if (s != Status::kOk) {
    std::fprintf(stderr, "transfer failed: %s (target may have been modified: %s)\n",
                 to_string(request->error().status),
                 request->error().may_have_modified_target ? "yes" : "no");
    return 1;
  }

  /* The peer filled its pool with a known pattern. */
  bool intact = true;
  for (size_t i = 0; i < pool.size(); ++i) {
    if (pool[i] != static_cast<uint8_t>(i * 7 + 3)) {
      std::fprintf(stderr, "mismatch at %zu\n", i);
      intact = false;
      break;
    }
  }
  std::printf("read %llu bytes: %s\n", (unsigned long long)kPoolBytes,
              intact ? "verified" : "CORRUPT");

  EngineStats const stats = engine->stats();
  std::printf("extra payload copied: %llu bytes\n",
              (unsigned long long)stats.payload_bytes_copied);

  send_blob(fd, "done", 4);
  ::close(fd);

  /* close() drains, and reports a timeout rather than returning while work is
   * still outstanding -- returning early would invite freeing memory the NIC
   * is still reading. */
  s = engine->close(5000);
  if (s != Status::kOk) return fail("close", s);
  return intact ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s server | client <ip>\n", argv[0]);
    return 2;
  }
  if (std::strcmp(argv[1], "server") == 0) return run_server("0.0.0.0");
  if (argc < 3) {
    std::printf("client needs the server's address\n");
    return 2;
  }
  return run_client(argv[2]);
}
