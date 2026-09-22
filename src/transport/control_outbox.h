// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_TRANSPORT_CONTROL_OUTBOX_H
#define HUX_TRANSPORT_CONTROL_OUTBOX_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "hux/status.h"

namespace hux {

/* The sending half of a control channel, for callers that must not block.
 *
 * Control messages are produced on the completion path: a write that lands
 * sends a handoff so the peer learns which bytes arrived. That path also
 * drives every other transfer on the engine, so a send that waits there
 * stops all of them. And it does wait: the peer reads its control socket
 * from its own progress loop, so a peer that is busy, or one that never
 * polls at all, leaves the socket full. A benchmark receiver that only held
 * memory was enough to fill it, after which one send sat in poll() for its
 * whole 30 s timeout and the engine did nothing for 30 seconds.
 *
 * So the socket is never waited on. A message is written if it fits and
 * queued if it does not, the queue is drained from the progress loop, and
 * once the queue is over its bound the message is refused -- reported to
 * the caller, which counts it, rather than paid for in stalled transfers.
 * Losing a handoff degrades the peer's knowledge; blocking degrades
 * everything.
 *
 * Messages are queued whole, so a partial write can never leave the peer's
 * reader half way through one message and reading the next as its body. */
class ControlOutbox {
 public:
  /* Bounded by bytes rather than messages, because the bound exists to cap
   * memory: control messages are a few dozen bytes each, so this is room
   * for tens of thousands of them. */
  static constexpr size_t kDefaultLimit = 1u << 20;

  void reset(int fd, size_t limit = kDefaultLimit);

  /* Takes the message if it can, without ever waiting on the socket.
   *
   * kOk means it was written or queued; the caller no longer owns it and
   * should not retry, which would duplicate it. kWouldBlock means the peer
   * is not reading and the backlog is full, so it was not taken at all.
   * kPeerDisconnected means the channel is finished. */
  Status post(std::vector<uint8_t> msg);

  /* Writes what the socket will take. Called from whatever drives progress;
   * safe to call when there is nothing queued. */
  Status flush();

  size_t backlog_bytes() const;
  size_t backlog_messages() const;
  bool broken() const;

 private:
  Status flush_locked();

  mutable std::mutex mu_;
  int fd_ = -1;
  size_t limit_ = kDefaultLimit;
  std::deque<std::vector<uint8_t>> q_;
  /* How much of q_.front() has gone out. The front is committed once any of
   * it has been written -- the peer is already reading it. */
  size_t front_off_ = 0;
  size_t bytes_ = 0;
  bool broken_ = false;
};

}  // namespace hux
#endif  // HUX_TRANSPORT_CONTROL_OUTBOX_H
