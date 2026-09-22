// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#include "transport/control_outbox.h"

#include <sys/socket.h>

#include <cerrno>

namespace hux {

void ControlOutbox::reset(int fd, size_t limit) {
  std::lock_guard<std::mutex> g(mu_);
  fd_ = fd;
  limit_ = limit;
  q_.clear();
  front_off_ = 0;
  bytes_ = 0;
  broken_ = false;
}

Status ControlOutbox::flush_locked() {
  if (broken_) return Status::kPeerDisconnected;
  if (fd_ < 0) return Status::kPeerDisconnected;
  while (!q_.empty()) {
    std::vector<uint8_t> const& m = q_.front();
    size_t const left = m.size() - front_off_;
    /* MSG_NOSIGNAL: a peer that closed its end turns a write into SIGPIPE,
     * and nothing here installs a handler. MSG_DONTWAIT because the socket
     * is not required to be non-blocking -- during the handshake it is
     * not, and this must not wait there either. */
    ssize_t const k =
        ::send(fd_, m.data() + front_off_, left, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (k > 0) {
      front_off_ += static_cast<size_t>(k);
      bytes_ -= static_cast<size_t>(k);
      if (front_off_ == m.size()) {
        q_.pop_front();
        front_off_ = 0;
      }
      continue;
    }
    if (k < 0 && errno == EINTR) continue;
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return Status::kWouldBlock;
    /* Anything else is the channel ending: EPIPE, ECONNRESET, a closed fd.
     * Whatever is queued will never arrive. */
    broken_ = true;
    return Status::kPeerDisconnected;
  }
  return Status::kOk;
}

Status ControlOutbox::post(std::vector<uint8_t> msg) {
  if (msg.empty()) return Status::kInvalidArgument;
  std::lock_guard<std::mutex> g(mu_);
  if (broken_ || fd_ < 0) return Status::kPeerDisconnected;

  /* Drain first, so a socket that was full a moment ago does not refuse a
   * message it now has room for. A full socket here is not an error. */
  Status const s = flush_locked();
  if (s == Status::kPeerDisconnected) return s;

  if (q_.empty()) {
    /* The common case: nothing queued, so write it straight out and queue
     * only what the socket would not take. */
    size_t off = 0;
    while (off < msg.size()) {
      ssize_t const k = ::send(fd_, msg.data() + off, msg.size() - off,
                               MSG_NOSIGNAL | MSG_DONTWAIT);
      if (k > 0) {
        off += static_cast<size_t>(k);
        continue;
      }
      if (k < 0 && errno == EINTR) continue;
      if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      broken_ = true;
      return Status::kPeerDisconnected;
    }
    if (off == msg.size()) return Status::kOk;
    /* Partly written. The peer is already reading this message, so the rest
     * of it is owed whatever the bound says -- refusing now would desync
     * the stream, which is worse than exceeding the bound by one message. */
    bytes_ += msg.size() - off;
    q_.push_back(std::move(msg));
    front_off_ = off;
    return Status::kOk;
  }

  if (bytes_ + msg.size() > limit_) return Status::kWouldBlock;
  bytes_ += msg.size();
  q_.push_back(std::move(msg));
  return Status::kOk;
}

Status ControlOutbox::flush() {
  std::lock_guard<std::mutex> g(mu_);
  return flush_locked();
}

size_t ControlOutbox::backlog_bytes() const {
  std::lock_guard<std::mutex> g(mu_);
  return bytes_;
}

size_t ControlOutbox::backlog_messages() const {
  std::lock_guard<std::mutex> g(mu_);
  return q_.size();
}

bool ControlOutbox::broken() const {
  std::lock_guard<std::mutex> g(mu_);
  return broken_;
}

}  // namespace hux
