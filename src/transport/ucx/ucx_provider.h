/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * UCX provider.
 *
 * The difference that matters is completion. A UCX put reports when the
 * source buffer is free to reuse, which is not when the data has reached the
 * peer -- that needs a flush. Treating the first as the second would let a
 * caller consume data the peer has not received, so this provider does not
 * report transfer_complete until the flush says so.
 *
 * All UCX calls for one worker happen on one thread. The library does not
 * serialize them, and a worker driven from two threads corrupts quietly. */
#ifndef HUX_TRANSPORT_UCX_PROVIDER_H
#define HUX_TRANSPORT_UCX_PROVIDER_H

#include <ucp/api/ucp.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "transport/provider.h"

namespace hux {

struct UcxConfig {
  /* Address peers dial back on; a provider cannot pick it on a multi-homed
   * host. */
  std::string advertise_ip = "127.0.0.1";
  uint16_t listen_port = 0;
};

class UcxConnection;

class UcxProvider : public TransportProvider {
 public:
  static Status create(UcxConfig const& cfg, std::shared_ptr<UcxProvider>* out);
  ~UcxProvider() override;

  ProviderCaps caps() const override;
  ProviderStats stats() const override;
  std::string describe() const override;

  Status register_region(void* addr, uint64_t length, DeviceId device,
                         AccessFlags access, uint64_t* local_key,
                         uint64_t* remote_key) override;
  Status deregister_region(uint64_t local_key) override;

  Status connect(std::vector<uint8_t> const& peer_metadata,
                 ProviderConnectionPtr* out) override;
  Status disconnect(ProviderConnectionPtr conn) override;
  Status local_metadata(std::vector<uint8_t>* out) const override;

  SubmitResult submit(ProviderConnection* conn,
                      std::vector<SubOp> const& ops) override;
  Status poll(uint32_t max_events, std::vector<CompletionEvent>* out) override;
  Status send_control(ProviderConnection* conn, uint16_t type,
                      std::vector<uint8_t> const& payload) override;
  Status poll_control(uint32_t max_items,
                      std::vector<ControlMessage>* out) override;
  Status poll_peer_arrivals(uint32_t max_items,
                            std::vector<PeerArrival>* out) override;
  Status flush(ProviderConnection* conn) override;
  Status drain(ProviderConnection* conn, int64_t timeout_ms) override;

  ucp_worker_h worker() const { return worker_; }
  ucp_context_h context() const { return context_; }

 private:
  UcxProvider() = default;
  Status init(UcxConfig const& cfg);

  UcxConfig cfg_;
  ucp_context_h context_ = nullptr;
  ucp_worker_h worker_ = nullptr;

  mutable std::mutex mu_;
  ProviderStats stats_;
  std::map<uint64_t, ucp_mem_h> memories_;
  std::map<uint64_t, std::vector<uint8_t>> rkey_blobs_;
  uint64_t next_key_ = 1;
  std::deque<CompletionEvent> completions_;
  std::deque<void*> inflight_;
  std::deque<ControlMessage> control_;
};

}  // namespace hux
#endif  // HUX_TRANSPORT_UCX_PROVIDER_H
