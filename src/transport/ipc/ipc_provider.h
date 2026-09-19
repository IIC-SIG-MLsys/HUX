/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Transfers between two processes on one host.
 *
 * The peer's memory is mapped into this process and the bytes are copied
 * across the mapping. That is a copy and is reported as one: nothing here is
 * zero-copy, and the saving over the network path is the network, not the
 * copy.
 *
 * Being on one host is not sufficient on its own. Host memory the caller
 * allocated cannot be exported at all, and two devices from different vendors
 * share no handle, so registration refuses rather than falling back to
 * something slower without saying so. */
#ifndef HUX_TRANSPORT_IPC_PROVIDER_H
#define HUX_TRANSPORT_IPC_PROVIDER_H

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "control/identity.h"
#include "hux/device.h"
#include "transport/provider.h"

namespace hux {

struct IpcConfig {
  /* Empty generates one in the abstract namespace, which leaves nothing
   * behind in the filesystem when the process dies. */
  std::string socket_name;
};

/* Bumped whenever the frame layout changes: an older peer has to be refused
 * rather than left to misread a frame it will accept. */
constexpr uint16_t kIpcWireMajor = 1;
constexpr uint16_t kIpcWireMinor = 0;

class IpcProvider : public TransportProvider {
 public:
  /* The backend does the vendor half: naming an allocation, mapping one, and
   * copying across the mapping. Without one this provider has nothing it can
   * do, so it is required rather than optional. */
  static Status create(IpcConfig const& cfg, std::shared_ptr<DeviceBackend> dev,
                       std::shared_ptr<IpcProvider>* out);
  ~IpcProvider() override;

  ProviderCaps caps() const override;
  ProviderStats stats() const override;
  std::string describe() const override;

  Status register_region(void* addr, uint64_t length, DeviceId device,
                         AccessFlags access, uint64_t* local_key,
                         uint64_t* remote_key) override;
  Status deregister_region(uint64_t local_key) override;

  Status connect(std::vector<uint8_t> const& peer_metadata,
                 ProviderConnectionPtr* out) override;
  /* Waits for one peer to dial in. Not part of the provider contract because
   * only the listening side needs it, the same as the RDMA provider. */
  Status accept(int64_t timeout_ms, ProviderConnectionPtr* out);
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

 private:
  /* A region of ours, published to the peer so it can map it. */
  struct Exported {
    void* addr = nullptr;
    uint64_t length = 0;
    IpcHandle handle;
  };

  /* A region of the peer's. The mapping is taken on first use rather than on
   * publication: a peer may export far more than this side ever reads, and
   * every mapping costs address space and a vendor handle. */
  struct Imported {
    uint64_t peer_addr = 0; /* base address in the peer's address space */
    uint64_t length = 0;
    IpcHandle handle;
    void* mapped = nullptr; /* null until first use */
  };

  Status handshake(int fd);
  /* Reads whatever has arrived without blocking. Frames the peer sent are
   * dispatched here; data completions are not, because a copy has finished by
   * the time submit returns. */
  Status pump(int fd);
  Status send_frame(int fd, uint16_t type, std::vector<uint8_t> const& body);
  void publish_all(int fd);
  /* Maps a published region, or returns the mapping already held. */
  Status ensure_mapped(uint64_t key, Imported** out);

  mutable std::mutex mu_;
  IpcConfig cfg_;
  std::shared_ptr<DeviceBackend> dev_;
  Identity peer_identity_;
  int listen_fd_ = -1;
  int sock_ = -1; /* one peer per provider, as the RDMA side has one per conn */
  std::string socket_name_;
  std::vector<uint8_t> inbox_; /* partial frame carried between pumps */

  ProviderStats stats_;
  std::deque<CompletionEvent> completions_;
  std::deque<ControlMessage> control_;
  std::map<uint64_t, Exported> exported_;
  std::map<uint64_t, Imported> imported_;
  std::map<uint64_t, bool> withdraw_acks_; /* key -> acknowledged */
  uint64_t next_key_ = 1;
  ProviderConnection* conn_ = nullptr; /* set once a peer is connected */
};

}  // namespace hux
#endif  // HUX_TRANSPORT_IPC_PROVIDER_H
