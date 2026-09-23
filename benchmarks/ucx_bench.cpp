/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The UCX baseline for hux-bench: the same measurement, driven through UCP
 * directly and linked against nothing of HUX. It is a comparison with UCX,
 * not with HUX running over UCX.
 *
 * Every definition is hux-bench's, so the two can be put side by side:
 * latency runs from submission to the moment the loop sees the operation
 * finished; percentiles are the sorted sample at floor(q * (n - 1)); Gb/s
 * comes from the median; rate is bytes over the wall clock of the whole run;
 * cores is processor time over wall time. Warm-up, depth, --for-seconds and
 * --start-at mean what they mean there.
 *
 * What "finished" means is the one thing that has to be chosen, because UCP
 * and HUX promise different things by default. A UCP put completes when its
 * source buffer may be reused -- for a short put that is before the bytes
 * have left the host. HUX's write completes when the peer's adapter has
 * placed them. So a put is followed by a flush of its endpoint and timed to
 * the flush, which is UCP's own way to learn the bytes arrived
 * (--completion remote, the default). --completion local times the put
 * alone, which is faster and promises less; it is there to be reported as
 * such, not compared as if it were the same thing.
 *
 * Two APIs, because they depend on the receiving host differently:
 *   --api put   one-sided RMA with an rkey, as NIXL uses it. The receiving
 *               CPU does nothing for the data.
 *   --api tag   two-sided tagged send. Above the rendezvous threshold the
 *               receiver must be progressing to take the data, and the send
 *               completes only once it has.
 *
 *   ./ucx-bench server [--port P] [--sizes a,b] [--api put|tag]
 *   ./ucx-bench client <ip> [--port P] [--sizes a,b] [--iters N]
 *               [--inflight D] [--warmup N] [--for-seconds S]
 *               [--start-at UNIX_MS] [--mix a,b] [--api put|tag]
 *               [--completion remote|local] [--split BYTES]
 *
 * The device, transports and GID index are UCX's to choose and are set the
 * UCX way, in the environment -- UCX_NET_DEVICES, UCX_TLS, UCX_IB_GID_INDEX.
 * What UCX actually selected for the endpoint is printed before the run. */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <ucp/api/ucp.h>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  bool server = false;
  std::string ip;
  uint16_t port = 18515;
  std::vector<uint64_t> sizes = {4096, 65536, 1u << 20, 8u << 20};
  std::vector<uint64_t> mix;
  int iters = 50;
  int warmup = 5;
  int inflight = 1;
  double for_seconds = 0;
  int64_t start_at_ms = 0;
  bool tag = false;
  bool remote_completion = true;
  /* Operations smaller than this go over a second endpoint -- a queue of
   * their own, which the adapter arbitrates against the first. It is what
   * a UCX application does about short messages stuck behind long ones, so
   * a baseline without it would be one nobody would deploy. 0 for one
   * endpoint. */
  uint64_t split = 0;
};

/* ---- the side channel, which carries addresses and nothing else ---- */

bool send_all(int fd, void const* p, size_t n) {
  auto const* b = static_cast<uint8_t const*>(p);
  while (n > 0) {
    ssize_t k = ::send(fd, b, n, MSG_NOSIGNAL);
    if (k <= 0) return false;
    b += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool recv_all(int fd, void* p, size_t n) {
  auto* b = static_cast<uint8_t*>(p);
  while (n > 0) {
    ssize_t k = ::recv(fd, b, n, 0);
    if (k <= 0) return false;
    b += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool send_blob(int fd, void const* p, uint32_t n) {
  return send_all(fd, &n, sizeof(n)) && send_all(fd, p, n);
}

bool recv_blob(int fd, std::vector<uint8_t>* out) {
  uint32_t n = 0;
  if (!recv_all(fd, &n, sizeof(n))) return false;
  out->resize(n);
  return n == 0 || recv_all(fd, out->data(), n);
}

/* Nagle off, on both ends and on the accepted socket too, which does not
 * inherit it. With it on, each small exchange waits 40 ms for an ACK and the
 * setup looks like a slow transport. */
void no_delay(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* ---- the measurement, copied from hux-bench so the two agree ---- */

double cpu_seconds() {
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
  auto const secs = [](timeval const& t) {
    return t.tv_sec + t.tv_usec / 1e6;
  };
  return secs(ru.ru_utime) + secs(ru.ru_stime);
}

struct Summary {
  double median_us = 0, p10_us = 0, p90_us = 0, p99_us = 0, gbps = 0;
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
  s.gbps = s.median_us > 0 ? (bytes * 8.0) / (s.median_us * 1e3) : 0;
  return s;
}

/* ---- UCX ---- */

struct Ucx {
  ucp_context_h ctx = nullptr;
  ucp_worker_h worker = nullptr;
  std::vector<uint8_t> addr;
  void* buf = nullptr;
  uint64_t bytes = 0;
  ucp_mem_h memh = nullptr;
  std::vector<uint8_t> rkey;

  bool init(bool tag, uint64_t pool_bytes, uint8_t stamp) {
    ucp_params_t p{};
    p.field_mask = UCP_PARAM_FIELD_FEATURES;
    p.features = UCP_FEATURE_RMA | (tag ? UCP_FEATURE_TAG : 0);
    ucp_config_t* cfg = nullptr;
    if (ucp_config_read(nullptr, nullptr, &cfg) != UCS_OK) return false;
    ucs_status_t s = ucp_init(&p, cfg, &ctx);
    ucp_config_release(cfg);
    if (s != UCS_OK) return false;

    ucp_worker_params_t wp{};
    wp.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    wp.thread_mode = UCS_THREAD_MODE_SINGLE;
    if (ucp_worker_create(ctx, &wp, &worker) != UCS_OK) return false;

    ucp_address_t* a = nullptr;
    size_t alen = 0;
    if (ucp_worker_get_address(worker, &a, &alen) != UCS_OK) return false;
    addr.assign(reinterpret_cast<uint8_t*>(a),
                reinterpret_cast<uint8_t*>(a) + alen);
    ucp_worker_release_address(worker, a);

    /* Registered once, as a whole, as hux-bench registers its pool: the
     * cost being compared is the transfer, not a registration per call. */
    bytes = pool_bytes;
    if (posix_memalign(&buf, 4096, bytes) != 0) return false;
    std::memset(buf, stamp, bytes);
    ucp_mem_map_params_t mp{};
    mp.field_mask =
        UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    mp.address = buf;
    mp.length = bytes;
    if (ucp_mem_map(ctx, &mp, &memh) != UCS_OK) return false;
    void* rb = nullptr;
    size_t rlen = 0;
    if (ucp_rkey_pack(ctx, memh, &rb, &rlen) != UCS_OK) return false;
    rkey.assign(static_cast<uint8_t*>(rb), static_cast<uint8_t*>(rb) + rlen);
    ucp_rkey_buffer_release(rb);
    return true;
  }

  void progress() {
    while (ucp_worker_progress(worker) != 0) {
    }
  }

  /* The local buffer's registration goes with every operation, so UCX does
   * not have to look it up or register on the fly. That is the fastest way
   * to call it, and the baseline should be the fastest honest one. */
  ucp_request_param_t param() const {
    ucp_request_param_t rp{};
    rp.op_attr_mask = UCP_OP_ATTR_FIELD_MEMH;
    rp.memh = memh;
    return rp;
  }

  ~Ucx() {
    if (memh != nullptr) ucp_mem_unmap(ctx, memh);
    if (worker != nullptr) ucp_worker_destroy(worker);
    if (ctx != nullptr) ucp_cleanup(ctx);
    std::free(buf);
  }
};

/* A handle is done when UCP says so; a null one finished in place. */
bool finished(void* h, ucs_status_t* st) {
  if (h == nullptr) {
    *st = UCS_OK;
    return true;
  }
  ucs_status_t const s = ucp_request_check_status(h);
  if (s == UCS_INPROGRESS) return false;
  *st = s;
  return true;
}

void release(void* h) {
  if (h != nullptr) ucp_request_free(h);
}

uint64_t biggest(Options const& o) {
  uint64_t b = 64;
  for (uint64_t s : o.sizes) b = std::max(b, s);
  for (uint64_t s : o.mix) b = std::max(b, s);
  return b;
}

/* ---- server ---- */

int run_server(Options const& o) {
  Ucx u;
  if (!u.init(o.tag, biggest(o), 0x5a)) {
    std::printf("[server] UCX init failed\n");
    return 1;
  }

  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(o.port);
  if (::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
      ::listen(srv, 1) != 0) {
    std::printf("[server] could not listen on :%u\n", o.port);
    return 1;
  }
  std::printf("[server] ready on :%u, %llu MiB registered, api %s\n", o.port,
              (unsigned long long)(u.bytes >> 20), o.tag ? "tag" : "put");
  std::fflush(stdout);

  int fd = ::accept(srv, nullptr, nullptr);
  if (fd < 0) return 1;
  no_delay(fd);
  uint64_t const base = reinterpret_cast<uintptr_t>(u.buf);
  std::vector<uint8_t> peer_addr;
  uint8_t const api = o.tag ? 1 : 0;
  if (!send_blob(fd, u.addr.data(), static_cast<uint32_t>(u.addr.size())) ||
      !send_blob(fd, u.rkey.data(), static_cast<uint32_t>(u.rkey.size())) ||
      !send_blob(fd, &base, sizeof(base)) ||
      !send_blob(fd, &u.bytes, sizeof(u.bytes)) ||
      !send_blob(fd, &api, sizeof(api)) || !recv_blob(fd, &peer_addr)) {
    std::printf("[server] exchange failed\n");
    return 1;
  }

  /* An endpoint back, which wireup needs and a tagged send's rendezvous
   * uses to answer. */
  ucp_ep_params_t ep{};
  ep.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
  ep.address = reinterpret_cast<ucp_address_t const*>(peer_addr.data());
  ucp_ep_h back = nullptr;
  if (ucp_ep_create(u.worker, &ep, &back) != UCS_OK) {
    std::printf("[server] endpoint back failed\n");
    return 1;
  }
  std::printf("[server] connected; progressing until the client finishes\n");
  std::fflush(stdout);

  /* Progressed for the whole run, as HUX's receiving side now is: a put
   * does not need it, but wireup does, and a tagged send cannot finish
   * without it. Receives are kept posted so a send never waits for one. */
  std::vector<void*> posted;
  auto post = [&] {
    ucp_request_param_t rp = u.param();
    posted.push_back(ucp_tag_recv_nbx(u.worker, u.buf, u.bytes, 1, 0, &rp));
  };
  if (o.tag)
    for (int i = 0; i < 64; ++i) post();

  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  while (true) {
    u.progress();
    if (o.tag) {
      for (auto& h : posted) {
        /* A receive that finished in place comes back null, and one that
         * failed comes back as an error; both leave the slot empty and both
         * need reposting. Reposting only the ones finished later let the
         * pool of posted receives shrink, and a sender then waited for a
         * receive that was not there -- a tail the harness made. */
        ucs_status_t st;
        bool const free = h == nullptr || UCS_PTR_IS_ERR(h) || finished(h, &st);
        if (!free) continue;
        if (h != nullptr && !UCS_PTR_IS_ERR(h)) release(h);
        ucp_request_param_t rp = u.param();
        h = ucp_tag_recv_nbx(u.worker, u.buf, u.bytes, 1, 0, &rp);
      }
    }
    if (::poll(&pfd, 1, 0) > 0) break; /* the client said it is done */
  }

  for (auto h : posted)
    if (h != nullptr && !UCS_PTR_IS_ERR(h)) {
      ucp_request_cancel(u.worker, h);
      release(h);
    }
  ucp_request_param_t cp{};
  cp.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
  cp.flags = UCP_EP_CLOSE_FLAG_FORCE;
  void* c = ucp_ep_close_nbx(back, &cp);
  ucs_status_t st;
  while (c != nullptr && !UCS_PTR_IS_ERR(c) && !finished(c, &st)) u.progress();
  release(UCS_PTR_IS_ERR(c) ? nullptr : c);
  ::close(fd);
  ::close(srv);
  std::printf("[server] done\n");
  return 0;
}

/* ---- client ---- */

struct Peer {
  ucp_ep_h ep = nullptr;
  ucp_rkey_h rkey = nullptr;
  /* The second endpoint, for --split. An rkey belongs to an endpoint, so it
   * is unpacked again for this one. */
  ucp_ep_h ep2 = nullptr;
  ucp_rkey_h rkey2 = nullptr;
  uint64_t base = 0;
  uint64_t bytes = 0;
};

int run_client(Options const& o) {
  Ucx u;
  if (!u.init(o.tag, biggest(o), 0x00)) {
    std::printf("UCX init failed\n");
    return 1;
  }

  int fd = -1;
  for (int i = 0; i < 60 && fd < 0; ++i) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(o.port);
    ::inet_pton(AF_INET, o.ip.c_str(), &a.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
      fd = s;
    } else {
      ::close(s);
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
  if (fd < 0) {
    std::printf("could not reach a server on %s:%u\n", o.ip.c_str(), o.port);
    return 1;
  }
  no_delay(fd);

  std::vector<uint8_t> saddr, srkey, sbase, sbytes, sapi;
  if (!recv_blob(fd, &saddr) || !recv_blob(fd, &srkey) ||
      !recv_blob(fd, &sbase) || !recv_blob(fd, &sbytes) ||
      !recv_blob(fd, &sapi) ||
      !send_blob(fd, u.addr.data(), static_cast<uint32_t>(u.addr.size()))) {
    std::printf("exchange failed\n");
    return 1;
  }
  /* A tagged send to a server that posts no receives waits for ever, with
   * nothing to say why, so a mismatch is refused here instead. */
  if (sapi.size() != 1 || (sapi[0] != 0) != o.tag) {
    std::printf("the server runs --api %s and this client --api %s\n",
                !sapi.empty() && sapi[0] ? "tag" : "put", o.tag ? "tag" : "put");
    return 1;
  }
  Peer peer;
  std::memcpy(&peer.base, sbase.data(), sizeof(peer.base));
  std::memcpy(&peer.bytes, sbytes.data(), sizeof(peer.bytes));

  ucp_ep_params_t ep{};
  ep.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
  ep.address = reinterpret_cast<ucp_address_t const*>(saddr.data());
  if (ucp_ep_create(u.worker, &ep, &peer.ep) != UCS_OK) {
    std::printf("ucp_ep_create failed\n");
    return 1;
  }
  if (ucp_ep_rkey_unpack(peer.ep, srkey.data(), &peer.rkey) != UCS_OK) {
    std::printf("ucp_ep_rkey_unpack failed\n");
    return 1;
  }
  if (o.split > 0) {
    if (ucp_ep_create(u.worker, &ep, &peer.ep2) != UCS_OK ||
        ucp_ep_rkey_unpack(peer.ep2, srkey.data(), &peer.rkey2) != UCS_OK) {
      std::printf("the second endpoint failed\n");
      return 1;
    }
  }
  /* Which endpoint an operation of n bytes goes over. */
  auto route = [&](uint64_t n) {
    bool const small = o.split > 0 && n < o.split;
    return std::make_pair(small ? peer.ep2 : peer.ep,
                          small ? peer.rkey2 : peer.rkey);
  };

  /* One operation, waited for. Used for warm-up and for the read-back. */
  auto one = [&](uint64_t n, bool write) -> ucs_status_t {
    ucp_request_param_t rp = u.param();
    void* h;
    auto const [e, k] = route(n);
    if (o.tag) {
      h = ucp_tag_send_nbx(e, u.buf, n, 1, &rp);
    } else if (write) {
      h = ucp_put_nbx(e, u.buf, n, peer.base, k, &rp);
    } else {
      h = ucp_get_nbx(e, u.buf, n, peer.base, k, &rp);
    }
    if (UCS_PTR_IS_ERR(h)) return UCS_PTR_STATUS(h);
    ucs_status_t st = UCS_OK;
    while (!finished(h, &st)) u.progress();
    release(h);
    if (st != UCS_OK || o.tag || !write || !o.remote_completion) return st;
    ucp_request_param_t fp{};
    void* f = ucp_ep_flush_nbx(e, &fp);
    if (UCS_PTR_IS_ERR(f)) return UCS_PTR_STATUS(f);
    while (!finished(f, &st)) u.progress();
    release(f);
    return st;
  };

  /* Wireup finishes on first use; done here so it is not charged to the
   * first measured operation of the first size. */
  if (one(64, !o.tag) != UCS_OK) {
    std::printf("the first operation failed\n");
    return 1;
  }

  if (o.start_at_ms > 0) {
    auto const target = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(o.start_at_ms));
    double const wait =
        std::chrono::duration<double>(target - std::chrono::system_clock::now())
            .count();
    if (wait > 0) {
      std::printf("waiting %.2f s to start together\n", wait);
      std::fflush(stdout);
      while (std::chrono::system_clock::now() < target)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
      std::printf("start time already passed by %.2f s; not lined up\n", -wait);
    }
  }

  /* What UCX actually chose for this endpoint -- the transport and the
   * device of every lane. Asking for rc on one adapter and silently getting
   * something else is how a baseline stops being the one described. */
  std::printf("\nucx %s, api %s, completion %s\n", ucp_get_version_string(),
              o.tag ? "tag" : "put",
              o.tag ? "send (above rendezvous: the peer took it)"
                    : (o.remote_completion ? "remote (put + endpoint flush)"
                                           : "local (put alone)"));
  std::fflush(stdout);
  ucp_ep_print_info(peer.ep, stdout);
  if (peer.ep2 != nullptr) {
    std::printf("second endpoint, for operations under %llu B:\n",
                (unsigned long long)o.split);
    ucp_ep_print_info(peer.ep2, stdout);
  }
  std::fflush(stdout);

  double last_cpu = 0, last_wall = 0;

  /* The same loop as hux-bench's run_schedule: up to `depth` outstanding, a
   * deadline instead of a count when one was asked for, and latency from
   * submission to the moment this loop sees the operation finished. */
  using Runs = std::map<uint64_t, std::vector<double>>;
  auto run_schedule = [&](std::vector<uint64_t> const& schedule, bool write,
                          int depth) -> std::pair<Runs, double> {
    Runs samples;
    for (uint64_t b : schedule) samples[b];
    struct Live {
      void* op;    /* the put, get or send */
      void* flush; /* the flush that settles a put, when asked for */
      Clock::time_point at;
      uint64_t bytes;
    };
    std::vector<Live> live;
    size_t submitted = 0, finished_n = 0;
    double const cpu0 = cpu_seconds();
    auto const start = Clock::now();
    auto const deadline =
        o.for_seconds > 0
            ? start + std::chrono::duration_cast<Clock::duration>(
                          std::chrono::duration<double>(o.for_seconds))
            : Clock::time_point::max();
    bool stopping = false;

    while (finished_n < schedule.size()) {
      if (!stopping && o.for_seconds > 0 && Clock::now() >= deadline) {
        stopping = true;
        finished_n += schedule.size() - submitted;
        submitted = schedule.size();
      }
      while (!stopping && static_cast<int>(live.size()) < depth &&
             submitted < schedule.size()) {
        uint64_t const n = schedule[submitted];
        auto const at = Clock::now();
        ucp_request_param_t rp = u.param();
        void* h;
        auto const [e, k] = route(n);
        if (o.tag) {
          h = ucp_tag_send_nbx(e, u.buf, n, 1, &rp);
        } else if (write) {
          h = ucp_put_nbx(e, u.buf, n, peer.base, k, &rp);
        } else {
          h = ucp_get_nbx(e, u.buf, n, peer.base, k, &rp);
        }
        if (UCS_PTR_IS_ERR(h)) {
          std::printf("submit failed: %s; stopping this run\n",
                      ucs_status_string(UCS_PTR_STATUS(h)));
          /* Give up the rest, as the deadline does. Only stopping would
           * leave the loop waiting for work that was never going to be
           * submitted. */
          stopping = true;
          finished_n += schedule.size() - submitted;
          submitted = schedule.size();
          break;
        }
        void* f = nullptr;
        if (!o.tag && write && o.remote_completion) {
          /* Flushes on one endpoint complete in order, and each covers every
           * operation issued before it, so this one completes when this put
           * and all earlier ones have reached the peer -- the same in-order
           * guarantee an RC queue gives HUX's write. */
          /* On the endpoint the put went over: with --split, a short put's
           * flush then waits only for the short ones ahead of it. */
          ucp_request_param_t fp{};
          f = ucp_ep_flush_nbx(e, &fp);
          if (UCS_PTR_IS_ERR(f)) {
            std::printf("flush failed: %s; stopping this run\n",
                        ucs_status_string(UCS_PTR_STATUS(f)));
            release(h);
            stopping = true;
            finished_n += schedule.size() - submitted;
            submitted = schedule.size();
            break;
          }
        }
        live.push_back({h, f, at, n});
        ++submitted;
      }
      u.progress();
      auto const now = Clock::now();
      for (auto it = live.begin(); it != live.end();) {
        ucs_status_t st_op = UCS_OK, st_fl = UCS_OK;
        bool const op_done = finished(it->op, &st_op);
        bool const fl_done = finished(it->flush, &st_fl);
        if (!op_done || !fl_done) {
          ++it;
          continue;
        }
        if (st_op == UCS_OK && st_fl == UCS_OK)
          samples[it->bytes].push_back(
              std::chrono::duration<double, std::micro>(now - it->at).count());
        release(it->op);
        release(it->flush);
        ++finished_n;
        it = live.erase(it);
      }
      if (live.empty() && submitted >= schedule.size()) break;
    }
    double const wall = std::chrono::duration<double>(Clock::now() - start).count();
    last_cpu = cpu_seconds() - cpu0;
    last_wall = wall;
    return {samples, wall};
  };

  std::printf("\n%-10s %-8s %10s %10s %10s %10s %10s %10s %7s %10s\n", "size",
              "op", "median_us", "p10_us", "p90_us", "p99_us", "Gb/s",
              "rate_Gb/s", "cores", "cpu_s/GiB");
  for (uint64_t bytes : o.sizes) {
    if (bytes > peer.bytes) {
      std::printf("%-10llu %-8s  skipped: larger than the peer's region\n",
                  (unsigned long long)bytes, "-");
      continue;
    }
    std::vector<bool> ops = o.tag ? std::vector<bool>{true}
                                  : std::vector<bool>{false, true};
    for (bool write : ops) {
      for (int i = 0; i < o.warmup; ++i) one(bytes, write);
      auto const [runs, wall] =
          run_schedule(std::vector<uint64_t>(o.iters, bytes), write, o.inflight);
      auto const& samples = runs.at(bytes);
      char const* name = o.tag ? "send" : (write ? "write" : "read");
      if (samples.empty()) {
        std::printf("%-10llu %-8s  no transfer succeeded\n",
                    (unsigned long long)bytes, name);
        continue;
      }
      Summary const sum = summarize(samples, bytes);
      double const rate =
          wall > 0 ? samples.size() * bytes * 8.0 / (wall * 1e9) : 0.0;
      double const gib = samples.size() * bytes / double(1 << 30);
      double const cores = last_wall > 0 ? last_cpu / last_wall : 0.0;
      std::printf("%-10llu %-8s %10.1f %10.1f %10.1f %10.1f %10.2f %10.2f"
                  " %7.2f %10.3f\n",
                  (unsigned long long)bytes, name, sum.median_us, sum.p10_us,
                  sum.p90_us, sum.p99_us, sum.gbps, rate, cores,
                  gib > 0 ? last_cpu / gib : 0.0);
      std::fflush(stdout);
    }
  }

  /* The same mixed stream as hux-bench: each size alone at the depth, then
   * round robin so every long operation has short ones on both sides. */
  bool mix_fits = true;
  for (uint64_t b : o.mix)
    if (b > peer.bytes) mix_fits = false;
  if (o.mix.size() >= 2 && !o.tag && !mix_fits)
    std::printf("\nmixed stream skipped: a size is larger than the peer's"
                " region (%llu B). Start the server with the same --mix.\n",
                (unsigned long long)peer.bytes);
  if (o.mix.size() >= 2 && !o.tag && mix_fits) {
    std::printf("\nmixed stream, %d in flight, write\n", o.inflight);
    std::printf("%-10s %12s %12s %12s %12s\n", "size", "alone_p50",
                "mixed_p50", "alone_p99", "mixed_p99");
    std::map<uint64_t, Summary> alone;
    for (uint64_t b : o.mix) {
      for (int i = 0; i < o.warmup; ++i) one(b, true);
      auto const [runs, wall] =
          run_schedule(std::vector<uint64_t>(o.iters, b), true, o.inflight);
      (void)wall;
      alone[b] = summarize(runs.at(b), b);
    }
    std::vector<uint64_t> schedule;
    for (int i = 0; i < o.iters; ++i)
      for (uint64_t b : o.mix) schedule.push_back(b);
    for (int i = 0; i < o.warmup; ++i)
      for (uint64_t b : o.mix) one(b, true);
    auto const [mixed, mwall] = run_schedule(schedule, true, o.inflight);
    uint64_t total = 0;
    for (uint64_t b : o.mix) {
      Summary const m = summarize(mixed.at(b), b);
      total += mixed.at(b).size() * b;
      std::printf("%-10llu %12.1f %12.1f %12.1f %12.1f\n",
                  (unsigned long long)b, alone[b].median_us, m.median_us,
                  alone[b].p99_us, m.p99_us);
    }
    std::printf("mixed stream rate: %.2f Gb/s, %.2f cores\n",
                mwall > 0 ? total * 8.0 / (mwall * 1e9) : 0.0,
                last_wall > 0 ? last_cpu / last_wall : 0.0);
  }

  /* Proof the path moved bytes rather than reporting completions for
   * nothing: a fresh pattern written to the peer, the local copy cleared,
   * and the pattern read back. Independent of the run above, whose writes
   * have long since replaced whatever the server put there. */
  if (!o.tag) {
    auto* b = static_cast<uint8_t*>(u.buf);
    for (int i = 0; i < 64; ++i) b[i] = static_cast<uint8_t>(0xc3 + i);
    bool ok = one(64, true) == UCS_OK;
    std::memset(b, 0, 64);
    ok = ok && one(64, false) == UCS_OK;
    for (int i = 0; ok && i < 64; ++i)
      if (b[i] != static_cast<uint8_t>(0xc3 + i)) ok = false;
    std::printf("\nwritten to the peer and read back: %s\n",
                ok ? "ok" : "WRONG");
  }

  ucp_rkey_destroy(peer.rkey);
  if (peer.rkey2 != nullptr) ucp_rkey_destroy(peer.rkey2);
  ucp_request_param_t cp{};
  cp.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
  cp.flags = UCP_EP_CLOSE_FLAG_FORCE;
  void* c = ucp_ep_close_nbx(peer.ep, &cp);
  ucs_status_t st;
  while (c != nullptr && !UCS_PTR_IS_ERR(c) && !finished(c, &st)) u.progress();
  release(UCS_PTR_IS_ERR(c) ? nullptr : c);
  if (peer.ep2 != nullptr) {
    void* c2 = ucp_ep_close_nbx(peer.ep2, &cp);
    while (c2 != nullptr && !UCS_PTR_IS_ERR(c2) && !finished(c2, &st))
      u.progress();
    release(UCS_PTR_IS_ERR(c2) ? nullptr : c2);
  }
  char x = 'x';
  send_all(fd, &x, 1);
  ::close(fd);
  return 0;
}

std::vector<uint64_t> parse_list(char const* s) {
  std::vector<uint64_t> out;
  std::string t(s);
  size_t pos = 0;
  while (pos < t.size()) {
    size_t comma = t.find(',', pos);
    out.push_back(std::strtoull(t.substr(pos, comma - pos).c_str(), nullptr, 10));
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 2) {
    std::printf(
        "usage: %s server|client <ip> [--port P] [--sizes a,b]"
        " [--iters N] [--inflight D] [--warmup N] [--for-seconds S]"
        " [--start-at UNIX_MS] [--mix a,b] [--api put|tag]"
        " [--completion remote|local] [--split BYTES]\n",
        argv[0]);
    return 1;
  }
  Options o;
  o.server = std::strcmp(argv[1], "server") == 0;
  int i = 2;
  if (!o.server) {
    if (argc < 3) return 1;
    o.ip = argv[2];
    i = 3;
  }
  for (; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    char const* v = argv[i + 1];
    if (k == "--port") o.port = static_cast<uint16_t>(std::atoi(v));
    else if (k == "--sizes") o.sizes = parse_list(v);
    else if (k == "--mix") o.mix = parse_list(v);
    else if (k == "--iters") o.iters = std::atoi(v);
    else if (k == "--warmup") o.warmup = std::atoi(v);
    else if (k == "--inflight") o.inflight = std::atoi(v);
    else if (k == "--for-seconds") o.for_seconds = std::atof(v);
    else if (k == "--start-at") o.start_at_ms = std::strtoll(v, nullptr, 10);
    else if (k == "--api") o.tag = std::strcmp(v, "tag") == 0;
    else if (k == "--completion") o.remote_completion = std::strcmp(v, "local") != 0;
    else if (k == "--split") o.split = std::strtoull(v, nullptr, 10);
    /* Accepted so a driver can pass hux-bench's arguments unchanged; UCX
     * takes its device from UCX_NET_DEVICES instead. */
    else if (k == "--local" || k == "--qp" || k == "--cc") continue;
    else {
      std::printf("unknown option %s\n", k.c_str());
      return 1;
    }
  }
  return o.server ? run_server(o) : run_client(o);
}
