/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * A peer that does not read its control channel must cost the sender
 * nothing but the messages it cannot deliver.
 *
 * This is a regression. Control messages are produced on the completion
 * path, and that path was allowed to wait for room in the socket. A
 * benchmark receiver that only held memory -- never polling, so never
 * reading -- filled the socket after a few thousand handoffs, and the next
 * send sat in poll() for its whole 30 s timeout with the engine stopped
 * behind it. Measured: a 12 s window carried about 2 s of transfers and
 * reported 3.47 Gb/s on a link doing 51.
 *
 * What is checked is that posting to a peer that never reads returns
 * promptly, that it starts refusing rather than growing without bound, and
 * that it recovers once the peer reads again. The timing check is loose --
 * it is there to catch a wait of tens of seconds, not to measure. */
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <vector>

#include "test_main.h"
#include "transport/control_outbox.h"

using namespace hux;

namespace {

/* The socket's own buffer is pinned, because what is being tested is what
 * happens once it is full. Left at the host's default, the case that fills
 * it is whatever that host happens to be configured for: this passed
 * everywhere until a machine with net.core.wmem_default of 203 MiB, a
 * thousand times the usual, swallowed every message and refused none.
 *
 * Not too small either. SO_SNDBUF is not payload capacity: each message on
 * a Unix socket is a buffer of its own, and the kernel counts its overhead
 * against the same limit, so 64 KiB of it holds about 11 KiB of 128-byte
 * messages. The value below leaves room for the backlogs these tests use
 * while staying far under any host's default. */
constexpr int kSocketBuffer = 256 << 10;

struct Pair {
  int tx = -1, rx = -1;
  Pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
      tx = fds[0];
      rx = fds[1];
      int const buf = kSocketBuffer;
      ::setsockopt(tx, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    }
  }

  /* What the kernel actually gave: it doubles the request, applies a
   * minimum, and a test that posts "more than the socket holds" has to know
   * how much that is. */
  int send_buffer() const {
    int v = 0;
    socklen_t n = sizeof(v);
    if (::getsockopt(tx, SOL_SOCKET, SO_SNDBUF, &v, &n) != 0) return 0;
    return v;
  }
  ~Pair() {
    if (tx >= 0) ::close(tx);
    if (rx >= 0) ::close(rx);
  }
};

std::vector<uint8_t> msg(size_t n, uint8_t fill = 0xab) {
  return std::vector<uint8_t>(n, fill);
}

}  // namespace

HUX_TEST(outbox_does_not_wait_on_a_peer_that_never_reads) {
  Pair p;
  CHECK(p.tx >= 0);
  ControlOutbox out;
  out.reset(p.tx, 64 << 10);

  /* Far more than the socket and the backlog together can hold, measured
   * from what the socket says it has rather than assumed. */
  int const budget = p.send_buffer() + (64 << 10);
  int const posts = budget / 64 + 4096;
  auto const t0 = std::chrono::steady_clock::now();
  size_t accepted = 0, refused = 0;
  for (int i = 0; i < posts; ++i) {
    Status const s = out.post(msg(64));
    if (s == Status::kOk)
      ++accepted;
    else if (s == Status::kWouldBlock)
      ++refused;
    else
      CHECK_STATUS(s, Status::kOk); /* disconnected is a failure here */
  }
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();

  /* The bug this is for took 30 s on a single post. */
  CHECK(ms < 2000);
  CHECK(refused > 0);
  CHECK(accepted > 0);
  /* Bounded: the backlog cannot have taken everything that was refused. */
  CHECK(out.backlog_bytes() <= (64 << 10) + 64);
  CHECK(!out.broken());
}

HUX_TEST(outbox_drains_and_accepts_again_once_the_peer_reads) {
  Pair p;
  CHECK(p.tx >= 0);
  ControlOutbox out;
  out.reset(p.tx, 16 << 10);

  while (out.post(msg(128)) == Status::kOk) {
  }
  CHECK(out.backlog_bytes() > 0);

  /* The peer catches up. */
  std::vector<uint8_t> sink(64 << 10);
  for (int i = 0; i < 4096; ++i) {
    ssize_t const n = ::recv(p.rx, sink.data(), sink.size(), MSG_DONTWAIT);
    if (n <= 0) break;
  }
  CHECK_STATUS(out.flush(), Status::kOk);
  CHECK_EQ(out.backlog_bytes(), size_t(0));
  CHECK_STATUS(out.post(msg(128)), Status::kOk);
}

HUX_TEST(outbox_delivers_whole_messages_in_order) {
  Pair p;
  CHECK(p.tx >= 0);
  ControlOutbox out;
  out.reset(p.tx);

  for (int i = 0; i < 16; ++i)
    CHECK_STATUS(out.post(msg(32, static_cast<uint8_t>(i))), Status::kOk);
  CHECK_STATUS(out.flush(), Status::kOk);

  std::vector<uint8_t> got(16 * 32);
  size_t have = 0;
  while (have < got.size()) {
    ssize_t const n = ::recv(p.rx, got.data() + have, got.size() - have, 0);
    if (n <= 0) break;
    have += static_cast<size_t>(n);
  }
  CHECK_EQ(have, got.size());
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 32; ++j) CHECK_EQ(int(got[i * 32 + j]), i);
}

HUX_TEST(outbox_reports_a_peer_that_went_away) {
  Pair p;
  CHECK(p.tx >= 0);
  ControlOutbox out;
  out.reset(p.tx);
  ::close(p.rx);
  p.rx = -1;

  /* The first write may succeed into the send buffer; one of them will not,
   * and after that every call says so rather than pretending. */
  Status s = Status::kOk;
  for (int i = 0; i < 64 && s != Status::kPeerDisconnected; ++i)
    s = out.post(msg(256));
  CHECK_STATUS(s, Status::kPeerDisconnected);
  CHECK(out.broken());
  CHECK_STATUS(out.post(msg(8)), Status::kPeerDisconnected);
}
