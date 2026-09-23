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
  b.offset = 1ull << 35; /* beyond 32 bits on both */
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

  bool setup(uint32_t queue_depth = 1024, MockConfig mc = {}) {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    cfg.notify_queue_depth = queue_depth;
    provider = std::make_shared<MockProvider>(mc);
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk)
      return false;
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
  CHECK_EQ(std::memcmp(notes[0].payload.data(), payload.data(), payload.size()),
           0);
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

HUX_TEST(a_notification_the_peer_cannot_queue_is_refused) {
  /* Depth 1: the second notification cannot be queued, so it must not be
   * confirmed either -- acknowledging a dropped message would tell the sender
   * something false. Nor can it go unanswered: the sender then waited for
   * good. It is refused, and fails saying why. */
  NotifyFixture f;
  CHECK(f.setup(1));

  std::vector<uint8_t> payload{9};
  RequestPtr first, second;
  CHECK_STATUS(f.engine->notify(f.peer.get(), payload, &first), Status::kOk);
  CHECK_STATUS(f.engine->notify(f.peer.get(), payload, &second), Status::kOk);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 200 && !(is_terminal(first->state()) &&
                               is_terminal(second->state()));
       ++i)
    f.engine->poll_completions(8, &done);

  CHECK(first->state() == RequestState::kSucceeded);
  CHECK(second->state() == RequestState::kFailed);
  CHECK_STATUS(second->error().status, Status::kResourceExhausted);
  CHECK_EQ(f.engine->stats().notifications_dropped, 1u);
}

namespace {

/* A notification whose acknowledgement is never coming: the peer has stopped
 * reading. What ends it has to be something on this side. */
bool unacknowledged(NotifyFixture* f, RequestPtr* req) {
  MockConfig mc;
  mc.swallow_control = true;
  if (!f->setup(1024, mc)) return false;
  if (f->engine->notify(f->peer.get(), {7}, req) != Status::kOk) return false;
  std::vector<RequestPtr> done;
  for (int i = 0; i < 20; ++i) f->engine->poll_completions(8, &done);
  return (*req)->state() == RequestState::kWaitNotifyAck;
}

}  // namespace

HUX_TEST(a_peer_that_goes_fails_the_notifications_it_never_acknowledged) {
  NotifyFixture f;
  RequestPtr req;
  CHECK(unacknowledged(&f, &req));

  f.provider->retire_connections();
  std::vector<RequestPtr> done;
  f.engine->poll_completions(8, &done);

  CHECK(req->state() == RequestState::kFailed);
  CHECK_STATUS(req->error().status, Status::kPeerDisconnected);
  /* Handed out like any other ending. */
  CHECK_EQ(done.size(), 1u);
  CHECK(done.size() == 1 && done[0] == req);
}

HUX_TEST(removing_a_peer_fails_the_notifications_it_never_acknowledged) {
  NotifyFixture f;
  RequestPtr req;
  CHECK(unacknowledged(&f, &req));

  CHECK_STATUS(f.engine->remove_peer(f.peer), Status::kOk);

  CHECK(req->state() == RequestState::kFailed);
  CHECK_STATUS(req->error().status, Status::kPeerDisconnected);
}

HUX_TEST(a_cancelled_notification_stops_waiting) {
  /* Its bytes are gone and cannot be called back; what cancelling can stop
   * is the wait, and a wait that ran on regardless was a request that never
   * ended. */
  NotifyFixture f;
  RequestPtr req;
  CHECK(unacknowledged(&f, &req));

  CHECK_STATUS(req->cancel(), Status::kOk);
  std::vector<RequestPtr> done;
  f.engine->poll_completions(8, &done);

  CHECK(req->state() == RequestState::kCancelled);
  CHECK_STATUS(req->wait(0), Status::kCancelled);
  CHECK(done.size() == 1 && done[0] == req);
}

HUX_TEST(close_cancels_the_notifications_still_unacknowledged) {
  /* Nothing reads an acknowledgement once the engine has stopped, so a
   * notification left waiting through close() never ended. */
  NotifyFixture f;
  RequestPtr req;
  CHECK(unacknowledged(&f, &req));

  CHECK_STATUS(f.engine->close(50), Status::kOk);

  CHECK(req->state() == RequestState::kCancelled);
  RequestPtr late;
  CHECK_STATUS(f.engine->notify(f.peer.get(), {8}, &late),
               Status::kInvalidArgument);
}
