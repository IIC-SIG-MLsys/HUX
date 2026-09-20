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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "core/region_impl.h"
#include "harness.h"
#include "hux/engine.h"
#include "transport/cc/controller.h"
#include "transport/rdma/rdma_provider.h"

using namespace hux;
using namespace hux::manual;

namespace {

constexpr uint16_t kMetaPort = 18515;

constexpr uint64_t kBytes = 1u << 20;

int run_server(int gpu, uint32_t qps, CongestionControllerPtr cc,
               std::string const& local_ip) {
  RdmaConfig cfg;
  /* The address this side is reached on. Across hosts it also selects the
   * port and GID, so it is the local address, never the peer's. */
  cfg.advertise_ip = local_ip;
  cfg.qp_per_conn = qps;
  cfg.cc = cc;
  std::shared_ptr<RdmaProvider> prov;
  if (RdmaProvider::create(cfg, &prov) != Status::kOk) {
    std::printf("provider create failed\n");
    return 1;
  }

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

  Buffer buf;
  if (!buf.make(gpu, kBytes)) {
    std::printf("no usable memory\n");
    return 1;
  }
  buf.fill(0x11); /* the client will read this, and later overwrite it */

  /* Registered through the engine so both ends agree on the region id: a
   * handoff names the region the peer imported, and only a region this engine
   * holds can be handed to a consumer. */
  MemoryRegionPtr region;
  if (engine->register_memory(
          buf.ptr, kBytes, AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
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
  std::printf("[server] waiting on :%u (%s memory)\n", kMetaPort,
              buf.on_device ? "device" : "host");
  int fd = ::accept(srv, nullptr, nullptr);

  send_blob(fd, meta.data(), static_cast<uint32_t>(meta.size()));
  send_blob(fd, desc.data(), static_cast<uint32_t>(desc.size()));

  ProviderConnectionPtr conn;
  if (prov->accept(20000, &conn) != Status::kOk) {
    std::printf("[server] rdma accept failed\n");
    return 1;
  }
  std::printf("[server] connected, holding memory\n");
  std::printf("[server] %s\n", prov->describe().c_str());

  std::vector<uint8_t> ack;
  recv_blob(fd, &ack); /* client says it is done reading and writing */

  std::vector<ReadyEventPtr> ready;
  for (int i = 0; i < 2000 && ready.empty(); ++i) {
    engine->poll_ready_events(8, &ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::printf("[server] ready handoffs received: %zu\n", ready.size());
  for (auto const& r : ready) {
    std::printf("[server]   request %llu region %llu gen %u span [%llu,%llu)\n",
                (unsigned long long)r->request(),
                (unsigned long long)r->region(), r->generation(),
                (unsigned long long)r->span().offset,
                (unsigned long long)(r->span().offset + r->span().length));
  }

  std::vector<Notification> notes;
  for (int i = 0; i < 2000 && notes.empty(); ++i) {
    engine->poll_notifications(8, &notes);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::printf("[server] notifications received: %zu\n", notes.size());
  if (!notes.empty()) {
    std::string text(notes[0].payload.begin(), notes[0].payload.end());
    std::printf("[server]   id %llu payload \"%s\"\n",
                (unsigned long long)notes[0].id, text.c_str());
  }

  std::printf("[server] verifying what the client wrote: %s\n",
              buf.verify(0x22) ? "OK" : "MISMATCH");
  send_blob(fd, "z", 1);
  ::close(fd);
  ::close(srv);
  return 0;
}

int run_client(std::string const& ip, int gpu, uint32_t qps,
               CongestionControllerPtr cc, std::string const& local_ip,
               int watch_peer_s) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kMetaPort);
  ::inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
  bool linked = false;
  for (int i = 0; i < 30 && !linked; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0)
      linked = true;
    else
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  /* Reported rather than assumed: reading the handshake off a socket that
   * never connected used to run on into an empty buffer and crash, which
   * says nothing about the server that is missing. */
  if (!linked) {
    std::printf("[client] no server answering at %s:%u\n", ip.c_str(),
                kMetaPort);
    return 1;
  }

  std::vector<uint8_t> meta, desc;
  recv_blob(fd, &meta);
  recv_blob(fd, &desc);
  if (meta.size() < 6 || desc.empty()) {
    std::printf("[client] server closed before sending its handshake\n");
    return 1;
  }

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
  cfg.advertise_ip = local_ip;
  cfg.qp_per_conn = qps;
  cfg.cc = cc;
  std::shared_ptr<RdmaProvider> prov;
  if (RdmaProvider::create(cfg, &prov) != Status::kOk) {
    std::printf("provider create failed\n");
    return 1;
  }

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kExplicit;
  ecfg.chunk_bytes = 256u << 10; /* forces several sub-operations */
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, nullptr, prov, &engine) != Status::kOk) {
    std::printf("engine create failed\n");
    return 1;
  }

  Buffer buf;
  if (!buf.make(gpu, kBytes)) {
    std::printf("no usable memory\n");
    return 1;
  }
  buf.fill(0x00);

  MemoryRegionPtr local;
  if (engine->register_memory(
          buf.ptr, kBytes, AccessFlags::kLocalRead | AccessFlags::kLocalWrite,
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
  std::printf("[client] connected (%s memory, %u queue pairs)\n",
              buf.on_device ? "device" : "host", peer->caps().qp_count);
  std::printf("[client] %s\n", prov->describe().c_str());

  auto drive = [&](RequestPtr const& req) {
    return hux::manual::drive(engine.get(), req);
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
  bool read_ok = s == Status::kOk && buf.verify(0x11);
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

#ifdef HUX_LOOPBACK_CUDA
  /* GPU produces, then the NIC may read. The dependency has to hold back the
   * transfer without holding back the calling thread -- both halves matter,
   * and a wrong answer on either is invisible without timing it. */
  if (buf.on_device) {
    std::printf("\n=== GPU dependency: produce, then transfer ===\n");
    std::shared_ptr<DeviceBackend> dev;
    if (CudaBackend::create(gpu, &dev) == Status::kOk) {
      cudaStream_t raw = nullptr;
      cudaStreamCreate(&raw);
      DeviceStreamPtr stream;
      dev->import_stream(raw, &stream);

      /* Enough work that the event is still outstanding when the transfer is
       * submitted. */
      void* scratch = nullptr;
      cudaMalloc(&scratch, 256u << 20);
      for (int i = 0; i < 8; ++i) cudaMemsetAsync(scratch, i, 256u << 20, raw);

      DeviceEventPtr ev;
      Status es = dev->record_event(stream.get(), &ev);
      std::printf("   event recorded: %s\n", to_string(es));

      TransferOptions opts;
      opts.after.push_back(ev);

      auto t0 = std::chrono::steady_clock::now();
      RequestPtr dep;
      Status ds = engine->write(peer.get(), lv, rv, opts, &dep);
      auto submit_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();

      EngineStats mid = engine->stats();
      std::printf("   submit returned in %lld us (status %s)\n",
                  (long long)submit_us, to_string(ds));
      std::printf("   state now: %s, waiting on dependency: %llu\n",
                  to_string(dep->state()),
                  (unsigned long long)mid.requests_waiting_on_dependency);

      Status fs = drive(dep);
      std::printf("   after the GPU work: %s, state %s\n", to_string(fs),
                  to_string(dep->state()));
      std::printf("   -> %s\n",
                  submit_us < 1000
                      ? "submission waited, the calling thread did not"
                      : "submit blocked: the caller was made to wait");

      cudaFree(scratch);
      cudaStreamDestroy(raw);
    }
  }
#endif

  std::printf("\n=== configuration in effect ===\n");
  std::printf("   %s\n", engine->describe().c_str());

  std::printf("\n=== payload copies on the whole path ===\n");
  {
    EngineStats st = engine->stats();
    std::printf("   requested %llu B across %llu sub-operations\n",
                (unsigned long long)st.payload_bytes,
                (unsigned long long)st.subops_posted);
    std::printf("   extra payload copied: %llu B\n",
                (unsigned long long)st.payload_bytes_copied);
    std::printf("   -> %s\n",
                st.payload_bytes_copied == 0
                    ? "zero-copy: the NIC used the caller's memory"
                    : "NOT zero-copy");
    std::printf("   sub-operations: posted=%llu completed=%llu failed=%llu%s\n",
                (unsigned long long)st.subops_posted,
                (unsigned long long)st.subops_completed,
                (unsigned long long)st.subops_failed,
                st.subops_posted == st.subops_completed + st.subops_failed
                    ? "   (balanced)"
                    : "   *** UNBALANCED ***");
    std::printf(
        "   congestion control: %s window=%llu B rate=%.1f MB/s,"
        " deferred=%llu\n",
        cc->name(), (unsigned long long)cc->window_bytes(CcDirection::kWrite),
        cc->rate_bytes_per_sec(CcDirection::kWrite) / 1e6,
        (unsigned long long)st.submit_deferred);
    std::printf("   registrations: created=%llu reused=%llu cached=%llu\n",
                (unsigned long long)st.registrations_created,
                (unsigned long long)st.registrations_reused,
                (unsigned long long)st.registration_cache_size);
    std::printf(
        "   control: notify sent=%llu recv=%llu dropped=%llu,"
        " handoff sent=%llu recv=%llu\n",
        (unsigned long long)st.notifications_sent,
        (unsigned long long)st.notifications_received,
        (unsigned long long)st.notifications_dropped,
        (unsigned long long)st.ready_handoffs_sent,
        (unsigned long long)st.ready_handoffs_received);
    std::printf("   peak in-flight requests: %llu\n",
                (unsigned long long)st.peak_inflight_requests);
    std::printf("   requests: accepted=%llu succeeded=%llu failed=%llu\n",
                (unsigned long long)st.requests_accepted,
                (unsigned long long)st.requests_succeeded,
                (unsigned long long)st.requests_failed);
  }

  std::printf("\n=== notification, acknowledged by the peer ===\n");
  {
    std::string text = "hello from the initiator";
    std::vector<uint8_t> payload(text.begin(), text.end());
    RequestPtr note;
    Status ns = engine->notify(peer.get(), payload, &note);
    /* Released before waiting: the acknowledgement can only come from a peer
     * that is running progress, and this one starts once it hears from us. */
    send_blob(fd, "x", 1);
    if (ns == Status::kOk) {
      Status fs = drive(note);
      std::printf("   send=%s  settle=%s  state=%s\n", to_string(ns),
                  to_string(fs), to_string(note->state()));
      std::printf("   -> %s\n", note->state() == RequestState::kSucceeded
                                    ? "the peer confirmed it reached its queue"
                                    : "not confirmed");
    } else {
      std::printf("   notify refused: %s\n", to_string(ns));
    }
  }

  /* Last, because it destroys the connection: an RC queue pair that takes a
   * fatal completion moves to ERROR and flushes everything posted after it.
   *
   * The case itself is the one that leaves UCCL's poll_async waiting forever
   * -- a read against an address the peer never exported. What matters is
   * whether the caller learns of it, and whether the request reaches a
   * terminal state so its memory can be released. */
  std::printf("\n=== failure is reported, not hung ===\n");
  {
    RegionDescriptor bogus;
    bogus.region = 99;
    bogus.generation = 1;
    bogus.base = 0xdead0000ull; /* never registered by the peer */
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
                      std::chrono::steady_clock::now() - t0)
                      .count();
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
  /* With --watch-peer N, sit and poll for N seconds so a peer that exits can
   * be noticed. The point is that nothing else happens in the meantime: the
   * connection simply goes, and the engine has to report it without a
   * transfer to fail against. */
  if (watch_peer_s > 0) {
    std::printf("\n=== watching the peer for %d s ===\n", watch_peer_s);
    std::fflush(stdout);
    bool seen = false;
    for (int i = 0; i < watch_peer_s * 10 && !seen; ++i) {
      std::vector<RequestPtr> drained;
      engine->poll_completions(8, &drained);
      if (!peer->connected()) {
        std::printf("   peer reported gone after %.1f s\n", i / 10.0);
        seen = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!seen) std::printf("   peer still reported as connected\n");
    std::fflush(stdout);
  }

  std::printf("\n=== repeated queries agree ===\n");
  bool d1 = false, d2 = false;
  wr->test(&d1);
  wr->test(&d2);
  std::printf("   test twice: %d %d   wait again: %s\n", d1, d2,
              to_string(wr->wait(100)));

  std::vector<uint8_t> fin;
  recv_blob(fd, &fin);
  ::close(fd);
  return read_ok && s == Status::kOk ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: %s server|client <ip> [--gpu N] [--qp N]\n"
        "       [--cc off|fixed:<bytes>|timely] [--local <ip>]\n",
        argv[0]);
    return 2;
  }
  int gpu = -1;
  for (int i = 1; i < argc - 1; ++i)
    if (std::strcmp(argv[i], "--gpu") == 0) gpu = std::atoi(argv[i + 1]);

  uint32_t qps = 1;
  for (int i = 1; i < argc - 1; ++i)
    if (std::strcmp(argv[i], "--qp") == 0)
      qps = static_cast<uint32_t>(std::atoi(argv[i + 1]));

  /* --cc off | fixed:<bytes>. Off is the comparison the others are measured
   * against, so it stays the default. */
  CongestionControllerPtr cc = make_cc_off();
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], "--cc") != 0) continue;
    std::string spec = argv[i + 1];
    if (spec.rfind("fixed:", 0) == 0)
      cc = make_cc_fixed_window(std::strtoull(spec.c_str() + 6, nullptr, 10));
    else if (spec == "timely")
      cc = make_cc_timely();
  }

  /* Across hosts each side names its own RoCE address; on one host the
   * default keeps everything on loopback. */
  std::string local_ip = "127.0.0.1";
  int watch_peer_s = 0;
  for (int i = 1; i < argc - 1; ++i)
    if (std::strcmp(argv[i], "--watch-peer") == 0)
      watch_peer_s = std::atoi(argv[i + 1]);
  for (int i = 1; i < argc - 1; ++i)
    if (std::strcmp(argv[i], "--local") == 0) local_ip = argv[i + 1];

  if (std::strcmp(argv[1], "server") == 0)
    return run_server(gpu, qps, cc, local_ip);
  if (argc < 3) {
    std::printf("client needs an ip\n");
    return 2;
  }
  return run_client(argv[2], gpu, qps, cc, local_ip, watch_peer_s);
}
