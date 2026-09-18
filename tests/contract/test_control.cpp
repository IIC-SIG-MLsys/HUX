/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Control channel framing and notification delivery, without hardware. The
 * mock loops messages straight back, so one engine plays both ends. */
#include <cstring>
#include <string>
#include <vector>

#include "control/control_message.h"
#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

/* ---- Framing ---- */

HUX_TEST(control_header_roundtrip) {
  ControlHeader h;
  h.type = ControlType::kNotification;
  h.flags = 0x1234;
  h.payload_len = 4096;
  uint8_t buf[kControlHeaderBytes];
  encode_control_header(h, buf);

  ControlHeader got;
  CHECK_STATUS(decode_control_header(buf, &got), Status::kOk);
  CHECK(got.type == ControlType::kNotification);
  CHECK_EQ(got.flags, 0x1234);
  CHECK_EQ(got.payload_len, 4096u);
}

HUX_TEST(control_header_refuses_major_mismatch) {
  ControlHeader h;
  uint8_t buf[kControlHeaderBytes];
  encode_control_header(h, buf);
  buf[0] = static_cast<uint8_t>(kControlMajor + 1);
  ControlHeader got;
  CHECK_STATUS(decode_control_header(buf, &got), Status::kUnsupported);
}

HUX_TEST(control_header_refuses_absurd_length) {
  /* A length field is whatever the peer says it is. Believing it would mean
   * allocating whatever was asked for. */
  ControlHeader h;
  h.payload_len = kControlMaxPayload + 1;
  uint8_t buf[kControlHeaderBytes];
  encode_control_header(h, buf);
  ControlHeader got;
  CHECK_STATUS(decode_control_header(buf, &got), Status::kInvalidArgument);
}

HUX_TEST(ready_handoff_roundtrip_keeps_64_bit_fields) {
  ReadyHandoffBody b;
  b.request = 0x1122334455667788ull;
  b.region = 42;
  b.generation = 7;
  b.offset = 1ull << 35;    /* beyond 32 bits on both */
  b.length = 1ull << 33;
  std::vector<uint8_t> buf;
  encode_ready_handoff(b, &buf);

  ReadyHandoffBody got;
  CHECK_STATUS(decode_ready_handoff(buf, &got), Status::kOk);
  CHECK_EQ(got.request, b.request);
  CHECK_EQ(got.region, b.region);
  CHECK_EQ(got.generation, b.generation);
  CHECK_EQ(got.offset, b.offset);
  CHECK_EQ(got.length, b.length);
}

HUX_TEST(ready_handoff_refuses_truncated_body) {
  ReadyHandoffBody b;
  std::vector<uint8_t> buf;
  encode_ready_handoff(b, &buf);
  buf.pop_back();
  ReadyHandoffBody got;
  CHECK_STATUS(decode_ready_handoff(buf, &got), Status::kInvalidArgument);
}

/* ---- Notifications ---- */

namespace {

struct NotifyFixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  PeerPtr peer;

  bool setup(uint32_t queue_depth = 1024) {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    cfg.notify_queue_depth = queue_depth;
    provider = std::make_shared<MockProvider>(MockConfig{});
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk) return false;
    std::vector<uint8_t> meta;
    engine->local_metadata(&meta);
    return engine->add_peer(meta, &peer) == Status::kOk;
  }
};

}  // namespace

HUX_TEST(notification_reaches_the_queue_with_its_payload) {
  NotifyFixture f;
  CHECK(f.setup());

  std::string text = "payload the peer defines";
  std::vector<uint8_t> payload(text.begin(), text.end());
  RequestPtr req;
  CHECK_STATUS(f.engine->notify(f.peer.get(), payload, &req), Status::kOk);

  std::vector<Notification> notes;
  for (int i = 0; i < 100 && notes.empty(); ++i)
    f.engine->poll_notifications(8, &notes);

  CHECK_EQ(notes.size(), 1u);
  CHECK_EQ(notes[0].payload.size(), payload.size());
  CHECK_EQ(std::memcmp(notes[0].payload.data(), payload.data(), payload.size()), 0);
}

HUX_TEST(notification_succeeds_only_once_acknowledged) {
  NotifyFixture f;
  CHECK(f.setup());

  std::vector<uint8_t> payload{1, 2, 3};
  RequestPtr req;
  CHECK_STATUS(f.engine->notify(f.peer.get(), payload, &req), Status::kOk);
  /* Sending only means the bytes left this host. Until the peer confirms it
   * queued them, the request has not succeeded. */
  CHECK(req->state() == RequestState::kWaitNotifyAck);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 200 && !is_terminal(req->state()); ++i)
    f.engine->poll_completions(8, &done);

  CHECK(req->state() == RequestState::kSucceeded);
}

HUX_TEST(oversized_notification_is_refused_up_front) {
  NotifyFixture f;
  CHECK(f.setup());
  EngineConfig const& cfg = f.engine->config();
  std::vector<uint8_t> payload(cfg.notify_max_payload + 1, 0);
  RequestPtr req;
  CHECK_STATUS(f.engine->notify(f.peer.get(), payload, &req),
               Status::kInvalidArgument);
}

HUX_TEST(a_full_notification_queue_is_not_acknowledged) {
  /* Depth 1: the second notification cannot be queued, so it must not be
   * confirmed either. Acknowledging a dropped message would tell the sender
   * something false, and back-pressure depends on it learning the truth. */
  NotifyFixture f;
  CHECK(f.setup(1));

  std::vector<uint8_t> payload{9};
  RequestPtr first, second;
  CHECK_STATUS(f.engine->notify(f.peer.get(), payload, &first), Status::kOk);
  CHECK_STATUS(f.engine->notify(f.peer.get(), payload, &second), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 200; ++i) f.engine->poll_completions(8, &done);

  CHECK(first->state() == RequestState::kSucceeded);
  CHECK(second->state() == RequestState::kWaitNotifyAck);
}
