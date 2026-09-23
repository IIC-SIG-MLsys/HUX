/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Transfers between engines in one process.
 *
 * Both sides can address the memory directly, so nothing goes near a NIC.
 * What it still has to do is behave like a transport: report completions the
 * same way, fail the same way, and refuse the same things. A local path that
 * quietly succeeds where a remote one would fail teaches callers habits that
 * break the moment the peer moves to another host. */
#ifndef HUX_TRANSPORT_LOCAL_PROVIDER_H
#define HUX_TRANSPORT_LOCAL_PROVIDER_H

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "transport/provider.h"

namespace hux {

/* Registrations are published here so a peer in the same process can resolve
 * a remote key. It stands in for what a NIC's protection domain does across
 * the network, and refuses an unknown key the same way. */
class LocalRegistry {
 public:
  static LocalRegistry& instance();

  uint64_t publish(void* addr, uint64_t length);
  void withdraw(uint64_t key);
  /* Checks that an address the peer named really belongs to that key, and
   * returns it. Addresses are absolute, the same convention RDMA uses, so the
   * two paths cannot disagree about what a descriptor means. Returns nullptr
   * for an unknown key or a range outside it -- the refusal a NIC gives,
   * rather than a wild pointer. */
  void* resolve(uint64_t key, uint64_t address, uint64_t length) const;
  /* Copies between local memory and a published range, holding the range
   * published until the copy is done: withdraw() waits for it. Resolving
   * and then copying left a window in which the owner could withdraw the
   * range and free the memory, and the copy then wrote into whatever the
   * allocator handed out next -- the one thing the IPC path waits for its
   * peer to rule out. False, and nothing copied, for an unknown key or a
   * range outside it. */
  bool copy(uint64_t key, uint64_t address, uint64_t length, void* local,
            bool into_local) const;

 private:
  struct Entry {
    void* addr;
    uint64_t length;
  };
  void* resolve_locked(uint64_t key, uint64_t address, uint64_t length) const;
  /* Shared for copies, exclusive to change the table. */
  mutable std::shared_mutex mu_;
  std::map<uint64_t, Entry> entries_;
  uint64_t next_ = 1;
};

class LocalProvider : public TransportProvider {
 public:
  static Status create(std::shared_ptr<LocalProvider>* out);

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

  /* Where this provider's peer should deliver control messages. Set once both
   * ends exist, since neither can be constructed knowing the other. */
  void pair_with(std::shared_ptr<LocalProvider> peer);

 private:
  friend class LocalConnection;
  void deliver_control(ControlMessage msg);

  mutable std::mutex mu_;
  ProviderStats stats_;
  std::deque<CompletionEvent> completions_;
  std::deque<ControlMessage> control_;
  std::map<uint64_t, void*> local_keys_;
  std::weak_ptr<LocalProvider> peer_;
};

}  // namespace hux
#endif  // HUX_TRANSPORT_LOCAL_PROVIDER_H
