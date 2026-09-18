/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_ENGINE_H
#define HUX_ENGINE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "hux/config.h"
#include "hux/device.h"
#include "hux/notification.h"
#include "hux/peer.h"
#include "hux/region.h"
#include "hux/request.h"
#include "hux/status.h"
#include "hux/types.h"

namespace hux {

struct TransferOptions {
  /* The NIC may touch the buffers only after these events complete. */
  std::vector<DeviceEventPtr> after;

  /* Published once the request reaches target_ready. A failed or cancelled
   * request never sends a success notification. */
  std::vector<uint8_t> notify_payload;
  bool notify = false;

  void* context = nullptr;  /* Local only. */
};

/* The engine owns no communication buffer. The application owns its memory and
 * the engine registers and transfers in place -- this is the main departure
 * from the ConnBuffer-centred design. Several engines may coexist in one
 * process with isolated resources. */
class Engine {
 public:
  virtual ~Engine() = default;

  static Status create(EngineConfig const& cfg,
                       std::shared_ptr<DeviceBackend> device,
                       std::unique_ptr<Engine>* out);

  virtual EngineConfig const& config() const = 0;

  /* Registration. Works on memory the application already owns; it is not
   * required to adopt a HUX allocator. */
  virtual Status register_memory(void* addr, uint64_t length,
                                 AccessFlags access,
                                 MemoryRegionPtr* out) = 0;
  virtual Status register_memory_batch(
      std::vector<void*> const& addrs, std::vector<uint64_t> const& lengths,
      AccessFlags access, std::vector<RegistrationResult>* out) = 0;
  /* Blocks new submissions first, then drains local use. */
  virtual Status deregister_memory(MemoryRegionPtr region) = 0;

  /* Peers. Re-adding a valid identity reuses the existing connection. */
  virtual Status local_metadata(std::vector<uint8_t>* out) const = 0;
  virtual Status add_peer(std::vector<uint8_t> const& metadata,
                          PeerPtr* out) = 0;
  virtual Status remove_peer(PeerPtr peer) = 0;

  /* Transfers, asynchronous by default. The scalar forms are single-segment
   * shortcuts over the same submission path. One vector operation targets one
   * peer; segments pair up by index with equal lengths. */
  virtual Status read(Peer* peer, RegionView const& local,
                      RegionView const& remote, TransferOptions const& opts,
                      RequestPtr* out) = 0;
  virtual Status write(Peer* peer, RegionView const& local,
                       RegionView const& remote, TransferOptions const& opts,
                       RequestPtr* out) = 0;
  virtual Status readv(Peer* peer, std::vector<RegionView> const& local,
                       std::vector<RegionView> const& remote,
                       TransferOptions const& opts, RequestPtr* out) = 0;
  virtual Status writev(Peer* peer, std::vector<RegionView> const& local,
                        std::vector<RegionView> const& remote,
                        TransferOptions const& opts, RequestPtr* out) = 0;

  /* Success means the peer engine queued it -- not that the peer application
   * handled it, and not that any data transfer completed. */
  virtual Status notify(Peer* peer, std::vector<uint8_t> const& payload,
                        RequestPtr* out) = 0;
  virtual Status poll_notifications(uint32_t max_items,
                                    std::vector<Notification>* out) = 0;

  virtual Status poll_completions(uint32_t max_items,
                                  std::vector<RequestPtr>* out) = 0;
  /* Ready handoffs for regions this engine is the write target of. */
  virtual Status poll_ready_events(uint32_t max_items,
                                   std::vector<ReadyEventPtr>* out) = 0;

  virtual Status progress() = 0;
  virtual Status record_event(DeviceStream* stream, DeviceEventPtr* out) = 0;

  /* Stops accepting work and drains. On timeout it keeps the resources and
   * reports the incomplete state; it never destroys objects still under DMA. */
  virtual Status close(int64_t timeout_ms) = 0;
};

}  // namespace hux
#endif  // HUX_ENGINE_H
