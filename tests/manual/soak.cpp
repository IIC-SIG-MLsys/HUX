/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The long run: every dimension at once, for as long as you give it.
 *
 * The other tests cross one dimension at a time -- several queue pairs, or
 * both directions, or several peers, or registration reuse. Each passes. What
 * has never been run is all of them together for hours, and that is where the
 * defects this library has actually had would show: a registration cache that
 * grows, a request that never reaches a terminal state, memory that is right
 * on the first round and wrong on the ten-thousandth.
 *
 * Every round verifies its bytes. A transport bug that is not a crash is a
 * wrong byte, and a soak that only counts completions would run for a day and
 * report success while moving rubbish.
 *
 * Two machines:
 *   soak server --local <ip> --port <p>          (one per peer)
 *   soak client <server-ip> --local <ip> --port <p> --peers N --hours H
 */
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "core/region_impl.h"
#include "harness.h"
#include "hux/engine.h"
#include "transport/rdma/rdma_provider.h"

using namespace hux;
using namespace hux::manual;
using Clock = std::chrono::steady_clock;

namespace {

/* Resident memory, in MiB. The counters below say how much work went
 * through; this says whether anything was kept. A run that moves correct
 * bytes for a day while this climbs has still found a defect, and without
 * it the only way to notice would be the machine running out. */
double rss_mib() {
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0.0;
  unsigned long total = 0, resident = 0;
  int const n = std::fscanf(f, "%lu %lu", &total, &resident);
  std::fclose(f);
  if (n != 2) return 0.0;
  long const page = ::sysconf(_SC_PAGESIZE);
  return double(resident) * double(page) / double(1 << 20);
}

/* Small and large in the same stream, so a short request has a long one in
 * front of it as it would in a real workload. */
constexpr uint64_t kSizes[] = {4096, 65536, 1 << 20, 4 << 20};

/* The peer's memory is split in two, which is what keeps several threads
 * from inventing failures for each other. The first half holds that peer's
 * pattern and is only ever read, so a reader always knows what it should
 * find. The second half is divided between the threads, each writing only
 * its own slice, so a write never disturbs what another thread is checking.
 *
 * Without the split a thread would have to write, verify, and put the
 * original back, and any reader arriving in between would report a
 * corruption that never happened. */
constexpr uint64_t kReadBytes = 4 << 20;
constexpr uint64_t kWriteBytes = 4 << 20;
constexpr uint64_t kPoolBytes = kReadBytes + kWriteBytes;

struct Options {
  std::string local_ip = "0.0.0.0";
  uint16_t port = 18700;
  int peers = 1;
  int threads = 4;
  double hours = 24.0;
  int gpu = -1;
  uint32_t qp = 1;
  /* How often to drop and remake a registration, in rounds. Zero disables
   * it. The cache is the thing being exercised: a soak that registers once
   * never touches the path where it grows. */
  int rereg_every = 500;
  int report_seconds = 300;
  /* Idle time between rounds, per thread. A soak is about duration and
   * coverage, not throughput: on a shared fabric it should be able to run
   * for hours without being the reason somebody else's job is slow. Zero
   * means go as fast as the link allows. */
  int pause_ms = 0;
};

std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_rounds{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint64_t> g_failures{0};
/* How often the engine asked for the work to be offered again. Reported
 * rather than treated as an error, so back-pressure is visible as what it
 * is. */
std::atomic<uint64_t> g_blocked{0};

void say_failure(char const* what, int peer, uint64_t size,
                 char const* detail) {
  /* Loud and immediate. A soak that logs a mismatch and carries on has
   * turned a reproducible failure into a statistic. */
  std::printf(
      "\n*** FAILED after %llu rounds: %s"
      " (peer %d, %llu bytes) %s\n",
      (unsigned long long)g_rounds.load(), what, peer, (unsigned long long)size,
      detail);
  std::fflush(stdout);
  g_failures.fetch_add(1);
  g_stop.store(true);
}

/* Submits, retrying while the engine says its queue is full.
 *
 * kWouldBlock is not a failure: the request was never accepted and had no
 * network side effect, and the contract says to retry it. Treating it as an
 * error is how a soak reports a defect after two million rounds of the
 * engine doing exactly what it promised -- which is what the first
 * overnight run did.
 *
 * Bounded, because retrying for ever would turn a genuinely stuck engine
 * into a hang rather than a failure. */
Status submit_retrying(Engine* engine, Peer* peer, RegionView const& lv,
                       RegionView const& rv, bool write, RequestPtr* out,
                       std::atomic<uint64_t>* blocked) {
  for (int i = 0; i < 20000; ++i) {
    Status const s = write ? engine->write(peer, lv, rv, {}, out)
                           : engine->read(peer, lv, rv, {}, out);
    if (s != Status::kWouldBlock) return s;
    blocked->fetch_add(1);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return Status::kWouldBlock;
}

int run_server(Options const& o) {
  RdmaConfig cfg;
  cfg.advertise_ip = o.local_ip;
  cfg.qp_per_conn = o.qp;
  std::shared_ptr<RdmaProvider> prov;
  if (RdmaProvider::create(cfg, &prov) != Status::kOk) {
    std::printf("provider create failed\n");
    return 1;
  }
  Buffer buf;
  if (!buf.make(o.gpu, kPoolBytes)) return 1;
  /* A pattern this port owns, so the client can tell whose memory it read. */
  uint8_t const seed = static_cast<uint8_t>(o.port & 0xff);
  buf.fill(seed);

  EngineConfig ecfg;
  /* A thread, because this side has nothing of its own to drive progress
   * with and the wait below is a blocking read. The control channel carries
   * the ready handoff for every write that lands here, and a peer whose
   * messages nobody reads fills it and starts losing them. */
  ecfg.progress = ProgressMode::kThread;
  std::unique_ptr<Engine> engine;
  if (make_engine(ecfg, buf.dev, prov, &engine) != Status::kOk) return 1;

  MemoryRegionPtr region;
  if (engine->register_memory(
          buf.ptr, kPoolBytes,
          AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite,
          &region) != Status::kOk)
    return 1;
  std::vector<uint8_t> meta, desc;
  if (engine->local_metadata(&meta) != Status::kOk) return 1;
  region->export_descriptor(&desc);

  int srv = listen_on(o.port);
  if (srv < 0) return 1;
  std::printf("[server :%u] ready, seed 0x%02x, %s memory\n", o.port, seed,
              buf.on_device ? "device" : "host");
  std::fflush(stdout);
  int fd = accept_one(srv);
  if (fd < 0) return 1;
  send_blob(fd, meta.data(), static_cast<uint32_t>(meta.size()));
  send_blob(fd, desc.data(), static_cast<uint32_t>(desc.size()));

  ProviderConnectionPtr conn;
  if (prov->accept(60000, &conn) != Status::kOk) {
    std::printf("[server :%u] rdma accept failed\n", o.port);
    return 1;
  }
  std::printf("[server :%u] connected; holding memory\n", o.port);
  std::fflush(stdout);

  /* The client drives the transfers; this side stays alive, keeps its memory
   * registered, and waits to be told the run is over.
   *
   * The waiting used to be this loop with engine->progress() in its body,
   * which never ran: recv_blob blocks until the client sends, so the body
   * was reached once, at the end. The comment said progress had to run and
   * the line above it stopped it -- for a whole run, until a counter for
   * refused handoffs made it visible. Progress is on its own thread now and
   * this only waits. */
  std::vector<uint8_t> bye;
  recv_blob(fd, &bye);
  std::printf("[server :%u] client finished\n", o.port);
  engine->close(5000);
  return 0;
}

struct PeerSlot {
  int fd = -1;
  uint16_t port = 0;
  uint8_t seed = 0;
  PeerPtr peer;
  RemoteRegionPtr remote;
};

int run_client(std::string const& ip, Options const& o) {
  RdmaConfig cfg;
  cfg.advertise_ip = o.local_ip;
  cfg.qp_per_conn = o.qp;
  std::shared_ptr<RdmaProvider> prov;
  if (RdmaProvider::create(cfg, &prov) != Status::kOk) return 1;

  EngineConfig ecfg;
  ecfg.progress = ProgressMode::kThread;
  std::unique_ptr<Engine> engine;
  std::vector<Buffer> scratch(o.threads);
  for (auto& b : scratch)
    if (!b.make(o.gpu, kPoolBytes)) return 1;
  if (make_engine(ecfg, scratch[0].dev, prov, &engine) != Status::kOk) return 1;

  std::vector<MemoryRegionPtr> local(o.threads);
  for (int t = 0; t < o.threads; ++t)
    if (engine->register_memory(
            scratch[t].ptr, kPoolBytes,
            AccessFlags::kLocalRead | AccessFlags::kLocalWrite,
            &local[t]) != Status::kOk)
      return 1;

  std::vector<PeerSlot> peers;
  for (int i = 0; i < o.peers; ++i) {
    PeerSlot s;
    s.port = static_cast<uint16_t>(o.port + i);
    s.seed = static_cast<uint8_t>(s.port & 0xff);
    s.fd = dial(ip, s.port, 60);
    if (s.fd < 0) {
      std::printf("no server on :%u\n", s.port);
      return 1;
    }
    std::vector<uint8_t> meta, desc;
    if (!recv_blob(s.fd, &meta) || !recv_blob(s.fd, &desc)) return 1;
    Status st = engine->add_peer(meta, &s.peer);
    if (st != Status::kOk) {
      std::printf("add_peer(:%u) -> %s\n", s.port, to_string(st));
      return 1;
    }
    st = s.peer->import_region(desc, &s.remote);
    if (st != Status::kOk) {
      std::printf("import_region(:%u) -> %s\n", s.port, to_string(st));
      return 1;
    }
    peers.push_back(std::move(s));
  }
  std::printf(
      "%d peers, %d threads, %s memory, %.1f hours, %d ms between"
      " rounds\n",
      o.peers, o.threads, scratch[0].on_device ? "device" : "host", o.hours,
      o.pause_ms);
  std::fflush(stdout);

  EngineStats const baseline = engine->stats();
  auto const started = Clock::now();
  auto const deadline =
      started + std::chrono::milliseconds(
                    static_cast<int64_t>(o.hours * 3600.0 * 1000.0));

  std::mutex reg_mu;
  std::vector<std::thread> workers;
  for (int t = 0; t < o.threads; ++t) {
    workers.emplace_back([&, t] {
      uint64_t round = 0;
      while (!g_stop.load() && Clock::now() < deadline) {
        PeerSlot& p = peers[(t + round) % peers.size()];
        bool const is_write = (round % 3) == 0;
        uint64_t size = kSizes[round % (sizeof(kSizes) / sizeof(*kSizes))];
        ++round;

        /* Reads come from the shared read half; writes go only into this
         * thread's own slice of the other half. */
        uint64_t const slice = kWriteBytes / static_cast<uint64_t>(o.threads);
        uint64_t const remote_at =
            is_write ? kReadBytes + slice * static_cast<uint64_t>(t) : 0;
        uint64_t const room = is_write ? slice : kReadBytes;
        if (size > room) size = room;

        RegionView lv, rv;
        if (local[t]->view(0, size, &lv) != Status::kOk ||
            p.remote->view(remote_at, size, &rv) != Status::kOk) {
          say_failure("view", p.port, size, "");
          return;
        }

        if (is_write) {
          /* Into this thread's own slice, which nothing else touches. The
           * write alone proves nothing about what landed, so it is read
           * back and compared. */
          uint8_t const mine = static_cast<uint8_t>(0xC0 + t);
          scratch[t].fill(mine);
          RequestPtr w;
          if (submit_retrying(engine.get(), p.peer.get(), lv, rv, true, &w,
                              &g_blocked) != Status::kOk ||
              w->wait(60000) != Status::kOk) {
            say_failure("write", p.port, size, "did not complete");
            return;
          }
          scratch[t].fill(0x00);
          RequestPtr r;
          if (submit_retrying(engine.get(), p.peer.get(), lv, rv, false, &r,
                              &g_blocked) != Status::kOk ||
              r->wait(60000) != Status::kOk) {
            say_failure("read back", p.port, size, "did not complete");
            return;
          }
          std::vector<uint8_t> got;
          scratch[t].read_back(&got);
          auto const& want = scratch[t].pattern(mine);
          if (std::memcmp(got.data(), want.data(), size) != 0) {
            say_failure("write then read back", p.port, size,
                        "bytes differ from what was written");
            return;
          }
        } else {
          scratch[t].fill(0x00);
          RequestPtr r;
          if (submit_retrying(engine.get(), p.peer.get(), lv, rv, false, &r,
                              &g_blocked) != Status::kOk ||
              r->wait(60000) != Status::kOk) {
            say_failure("read", p.port, size, "did not complete");
            return;
          }
          /* Whose bytes came back. Every peer numbers its regions from one,
           * so a request landing on the wrong peer returns the right length
           * and only this says otherwise. */
          std::vector<uint8_t> got;
          scratch[t].read_back(&got);
          auto const& want = scratch[t].pattern(p.seed);
          if (std::memcmp(got.data(), want.data(), size) != 0) {
            say_failure("read", p.port, size,
                        "bytes are not this peer's pattern");
            return;
          }
        }

        g_bytes.fetch_add(size);
        g_rounds.fetch_add(1);
        if (o.pause_ms > 0)
          std::this_thread::sleep_for(std::chrono::milliseconds(o.pause_ms));

        /* Registration churn, so the reuse cache is exercised rather than
         * filled once and left alone. Serialised: two threads dropping the
         * region another is submitting against is a different test. */
        if (o.rereg_every > 0 && (round % o.rereg_every) == 0) {
          std::lock_guard<std::mutex> g(reg_mu);
          MemoryRegionPtr fresh;
          if (engine->register_memory(
                  scratch[t].ptr, kPoolBytes,
                  AccessFlags::kLocalRead | AccessFlags::kLocalWrite,
                  &fresh) == Status::kOk) {
            MemoryRegionPtr old = local[t];
            local[t] = fresh;
            engine->deregister_memory(old);
          }
        }
      }
    });
  }

  /* Reporting, and the check that matters as much as the bytes: what the
   * engine is holding should not grow. A soak that moves correct data for a
   * day while its in-flight table climbs has still found a defect. */
  uint64_t last_rounds = 0;
  while (!g_stop.load() && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto const now = Clock::now();
    static auto last_report = started;
    if (now - last_report < std::chrono::seconds(o.report_seconds)) continue;
    last_report = now;

    EngineStats const st = engine->stats();
    double const hours =
        std::chrono::duration<double>(now - started).count() / 3600.0;
    uint64_t const rounds = g_rounds.load();
    std::printf(
        "%6.2f h  %10llu rounds (+%llu)  %8.1f GiB  "
        "accepted=%llu succeeded=%llu failed=%llu cancelled=%llu  "
        "copied=%llu  |  rss=%.0f MiB regs=%llu peak_inflight=%llu "
        "handoffs_lost=%llu\n",
        hours, (unsigned long long)rounds,
        (unsigned long long)(rounds - last_rounds),
        g_bytes.load() / double(1 << 30),
        (unsigned long long)(st.requests_accepted - baseline.requests_accepted),
        (unsigned long long)(st.requests_succeeded -
                             baseline.requests_succeeded),
        (unsigned long long)(st.requests_failed - baseline.requests_failed),
        (unsigned long long)(st.requests_cancelled -
                             baseline.requests_cancelled),
        (unsigned long long)(st.payload_bytes_copied -
                             baseline.payload_bytes_copied),
        /* Held rather than done: these are the ones that answer whether a
         * day of correct transfers left anything behind. */
        rss_mib(), (unsigned long long)st.registration_cache_size,
        (unsigned long long)st.peak_inflight_requests,
        (unsigned long long)(st.ready_handoffs_failed -
                             baseline.ready_handoffs_failed));
    std::fflush(stdout);
    last_rounds = rounds;

    if (st.requests_failed > baseline.requests_failed) {
      say_failure("engine counted a failed request", 0, 0, "");
      break;
    }
    if (st.payload_bytes_copied > baseline.payload_bytes_copied) {
      say_failure("a transfer was staged rather than done in place", 0, 0, "");
      break;
    }
  }

  g_stop.store(true);
  for (auto& w : workers) w.join();

  EngineStats const st = engine->stats();
  double const hours =
      std::chrono::duration<double>(Clock::now() - started).count() / 3600.0;
  std::printf("\n%.2f hours, %llu rounds, %.1f GiB, %llu failures\n", hours,
              (unsigned long long)g_rounds.load(),
              g_bytes.load() / double(1 << 30),
              (unsigned long long)g_failures.load());
  std::printf("accepted=%llu succeeded=%llu failed=%llu copied=%llu\n",
              (unsigned long long)st.requests_accepted,
              (unsigned long long)st.requests_succeeded,
              (unsigned long long)st.requests_failed,
              (unsigned long long)st.payload_bytes_copied);

  for (auto& p : peers) {
    send_blob(p.fd, "x", 1);
    ::close(p.fd);
  }
  engine->close(10000);
  return g_failures.load() == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: %s server|client <ip> [--local IP] [--port P]\n"
        "       [--peers N] [--threads N] [--hours H] [--gpu N]\n"
        "       [--qp N] [--rereg-every N] [--report-seconds N]\n",
        argv[0]);
    return 2;
  }
  Options o;
  std::string const mode = argv[1];
  std::string ip = (argc > 2 && argv[2][0] != '-') ? argv[2] : "";
  for (int i = 2; i + 1 < argc; ++i) {
    std::string const k = argv[i];
    if (k == "--local")
      o.local_ip = argv[i + 1];
    else if (k == "--port")
      o.port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
    else if (k == "--peers")
      o.peers = std::max(1, std::atoi(argv[i + 1]));
    else if (k == "--threads")
      o.threads = std::max(1, std::atoi(argv[i + 1]));
    else if (k == "--hours")
      o.hours = std::atof(argv[i + 1]);
    else if (k == "--gpu")
      o.gpu = std::atoi(argv[i + 1]);
    else if (k == "--qp")
      o.qp = static_cast<uint32_t>(std::atoi(argv[i + 1]));
    else if (k == "--rereg-every")
      o.rereg_every = std::atoi(argv[i + 1]);
    else if (k == "--pause-ms")
      o.pause_ms = std::max(0, std::atoi(argv[i + 1]));
    else if (k == "--report-seconds")
      o.report_seconds = std::max(1, std::atoi(argv[i + 1]));
  }
  if (mode == "server") return run_server(o);
  if (mode == "client" && !ip.empty()) return run_client(ip, o);
  std::printf("client needs the server's address\n");
  return 2;
}
