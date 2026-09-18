/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_NOTIFICATION_H
#define HUX_NOTIFICATION_H

#include <cstdint>
#include <memory>
#include <vector>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

class DeviceStream;

/* Anything the peer needs must travel in the payload; Request::context() is
 * local only. */
struct Notification {
  PeerId peer = 0;
  Epoch epoch = 0;
  NotificationId id = 0;
  RequestId related_request = 0;  /* 0 if not tied to a data transfer. */
  std::vector<uint8_t> payload;
};

/* kIndeterminate is distinct from kFailed on purpose: after a disconnect the
 * sender cannot tell whether the peer received it, so it must not simply
 * resend. */
enum class DeliveryState : uint8_t {
  kPending = 0,
  kDelivered,      /* Queued at the peer engine, not necessarily handled. */
  kFailed,
  kIndeterminate,
};

char const* to_string(DeliveryState s);

/* Handed to the target owner after a write arrives. The receiver builds its
 * own local dependency from this; the sender's stream says nothing about the
 * receiver's. */
class ReadyEvent {
 public:
  virtual ~ReadyEvent() = default;

  virtual RequestId request() const = 0;
  virtual PeerId peer() const = 0;
  virtual RegionId region() const = 0;
  virtual Generation generation() const = 0;
  virtual Span span() const = 0;
  virtual Status wait_on(DeviceStream* stream) = 0;
};

using ReadyEventPtr = std::shared_ptr<ReadyEvent>;

}  // namespace hux
#endif  // HUX_NOTIFICATION_H
