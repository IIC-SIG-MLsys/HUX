/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_CORE_READY_EVENT_IMPL_H
#define HUX_CORE_READY_EVENT_IMPL_H

#include "hux/device.h"
#include "hux/notification.h"

namespace hux {

/* Handed to the target owner once a peer's write has landed and this side has
 * done whatever its device needs for the bytes to be visible.
 *
 * Which bytes arrived comes from the control message, not the 32-bit
 * immediate that signals arrival: the immediate can name a handoff but not
 * describe one. */
class ReadyEventImpl : public ReadyEvent {
 public:
  ReadyEventImpl(RequestId req, PeerId peer, DeviceBackend* device, void* addr,
                 uint64_t bytes, RegionId region = 0, Generation gen = 0,
                 Span span = Span{})
      : req_(req),
        peer_(peer),
        device_(device),
        addr_(addr),
        bytes_(bytes),
        region_(region),
        gen_(gen),
        span_(span) {}

  RequestId request() const override { return req_; }
  PeerId peer() const override { return peer_; }
  RegionId region() const override { return region_; }
  Generation generation() const override { return gen_; }
  Span span() const override { return span_; }

  Status wait_on(DeviceStream* stream) override {
    if (stream == nullptr) return Status::kInvalidArgument;
    if (device_ == nullptr) return Status::kUnsupported;
    /* The receiver builds its own dependency here. The sender's stream says
     * nothing about this one. */
    return device_->make_visible(stream, addr_, bytes_);
  }

 private:
  RequestId req_;
  PeerId peer_;
  DeviceBackend* device_;
  void* addr_;
  uint64_t bytes_;
  RegionId region_;
  Generation gen_;
  Span span_;
};

}  // namespace hux
#endif  // HUX_CORE_READY_EVENT_IMPL_H
