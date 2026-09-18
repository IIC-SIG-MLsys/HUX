/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
//
/* Hardware-free provider. Its point is not to pretend to move data, but to
 * make the situations that are hard to stage on real hardware deterministic:
 * out-of-order completions, partial submits, one CQ batch mixing several
 * requests. Those are exactly where earlier implementations went wrong. */
#ifndef HUX_TRANSPORT_MOCK_PROVIDER_H
#define HUX_TRANSPORT_MOCK_PROVIDER_H

#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <vector>

#include "transport/provider.h"

namespace hux {

struct MockConfig {
  uint32_t qp_count = 1;
  uint32_t submit_capacity = 1024;
  /* Cap on accepted SubOps per submit, 0 for no cap; stages partial posts. */
  uint32_t accept_limit = 0;
  Status submit_status_on_partial = Status::kResourceExhausted;
  /* Shuffles completions to prove core does not rely on CQE order. */
  bool shuffle_completions = false;
  /* Makes completions report an error. */
  bool fail_subops = false;
  Status subop_error = Status::kTransportError;
  /* Actually move bytes so tests can verify content end to end. */
  bool move_data = true;
  /* Cap on bytes in flight, 0 for none. Stands in for a congestion window so
   * contention between requests can be reproduced without hardware. */
  uint64_t budget_bytes = 0;
  /* Accept work and never complete it, so a close that must report a timeout
   * has something outstanding to wait on. */
  bool never_complete = false;
};

class MockConnection : public ProviderConnection {
 public:
  explicit MockConnection(MockConfig const& c) : cfg_(c) {}
  uint32_t qp_count() const override { return cfg_.qp_count; }
  uint32_t submit_capacity() const override { return cfg_.submit_capacity; }

 private:
  MockConfig cfg_;
};

class MockProvider : public TransportProvider {
 public:
  explicit MockProvider(MockConfig cfg = {}) : cfg_(cfg), rng_(12345) {}

  ProviderStats stats() const override {
    std::lock_guard<std::mutex> g(mu_);
    return stats_;
  }

  ProviderCaps caps() const override {
    ProviderCaps c;
    c.name = "mock";
    c.supports_read = true;
    c.supports_write = true;
    c.supports_vector = true;
    c.supports_multi_qp = cfg_.qp_count > 1;
    c.supports_peer_signal = true;
    c.max_sge = 16;
    return c;
  }

  Status register_region(void* addr, uint64_t length, DeviceId, AccessFlags,
                         uint64_t* local_key, uint64_t* remote_key) override {
    std::lock_guard<std::mutex> g(mu_);
    uint64_t key = next_key_++;
    ++total_registrations_;
    regions_[key] = {addr, length};
    *local_key = key;
    /* Deliberately rkey != lkey, so that exporting the lkey by mistake fails
     * here rather than on the one device where they happen to match. */
    *remote_key = key + kRemoteKeyOffset;
    return Status::kOk;
  }

  Status deregister_region(uint64_t local_key) override {
    std::lock_guard<std::mutex> g(mu_);
    return regions_.erase(local_key) > 0 ? Status::kOk : Status::kNotFound;
  }

  Status connect(std::vector<uint8_t> const&,
                 ProviderConnectionPtr* out) override {
    *out = std::make_shared<MockConnection>(cfg_);
    return Status::kOk;
  }
  Status disconnect(ProviderConnectionPtr) override { return Status::kOk; }
  Status local_metadata(std::vector<uint8_t>* out) const override {
    *out = {'m', 'o', 'c', 'k'};
    return Status::kOk;
  }

  SubmitResult submit(ProviderConnection*,
                      std::vector<SubOp> const& ops) override;
  Status poll(uint32_t max_events, std::vector<CompletionEvent>* out) override;
  Status send_control(ProviderConnection* conn, uint16_t type,
                      std::vector<uint8_t> const& payload) override {
    std::lock_guard<std::mutex> g(mu_);
    /* Loops straight back: one engine plays both ends in these tests, which
     * is enough to exercise framing and dispatch. The connection is carried
     * through, because a reply has to go back the way it came -- dropping it
     * here would make replies untestable without hardware. */
    control_.push_back(ControlMessage{0, type, payload, conn});
    return Status::kOk;
  }

  Status poll_control(uint32_t max_items,
                      std::vector<ControlMessage>* out) override {
    if (out == nullptr) return Status::kInvalidArgument;
    out->clear();
    std::lock_guard<std::mutex> g(mu_);
    while (!control_.empty() && out->size() < max_items) {
      out->push_back(std::move(control_.front()));
      control_.pop_front();
    }
    return Status::kOk;
  }

  Status poll_peer_arrivals(uint32_t max_items,
                            std::vector<PeerArrival>* out) override {
    if (out == nullptr) return Status::kInvalidArgument;
    out->clear();
    std::lock_guard<std::mutex> g(mu_);
    while (!arrivals_.empty() && out->size() < max_items) {
      out->push_back(arrivals_.front());
      arrivals_.pop_front();
    }
    return Status::kOk;
  }

  Status flush(ProviderConnection*) override { return Status::kOk; }
  Status drain(ProviderConnection*, int64_t) override { return Status::kOk; }

  /* Registrations ever made, and how many are still live. The first shows
   * whether a range was reused; the second whether it was released. */
  uint64_t registration_count() const {
    std::lock_guard<std::mutex> g(mu_);
    return total_registrations_;
  }
  uint64_t live_registrations() const {
    std::lock_guard<std::mutex> g(mu_);
    return regions_.size();
  }

  /* Test helpers. */
  size_t pending() const {
    std::lock_guard<std::mutex> g(mu_);
    return pending_.size();
  }
  uint64_t submitted_subops() const {
    std::lock_guard<std::mutex> g(mu_);
    return submitted_;
  }

 private:
  static constexpr uint64_t kRemoteKeyOffset = 0x1000000;
  struct Reg {
    void* addr;
    uint64_t length;
  };

  mutable std::mutex mu_;
  MockConfig cfg_;
  std::mt19937 rng_;
  std::map<uint64_t, Reg> regions_;
  std::deque<CompletionEvent> pending_;
  std::deque<PeerArrival> arrivals_;
  std::deque<ControlMessage> control_;
  uint64_t next_key_ = 1;
  uint64_t submitted_ = 0;
  uint64_t inflight_bytes_ = 0;
  uint64_t total_registrations_ = 0;
  ProviderStats stats_;
};

}  // namespace hux
#endif  // HUX_TRANSPORT_MOCK_PROVIDER_H
