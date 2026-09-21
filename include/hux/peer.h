/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_PEER_H
#define HUX_PEER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hux/region.h"
#include "hux/status.h"
#include "hux/types.h"

namespace hux {

/* The selected path must be queryable, otherwise a fallback shows up only as
 * unexplained slowness. */
enum class PathKind : uint8_t {
  kUnknown = 0,
  kSameProcess,
  kIpc,
  kRdma,
  kUcx,
};

char const* to_string(PathKind p);

/* Where the peer is, which is a different question from how it is reached. A
 * peer in the next process can still be served over the network when no
 * closer transport is available, and a caller asking which it got must not be
 * given the better answer. */
enum class PeerPlace : uint8_t {
  kUnknown = 0,
  kSameProcess,
  kSameHost,
  kAnotherHost,
};

char const* to_string(PeerPlace p);

struct PeerCaps {
  /* The transport actually in use. */
  PathKind path = PathKind::kUnknown;
  /* Where the peer turned out to be. */
  PeerPlace place = PeerPlace::kUnknown;
  std::string provider;
  uint32_t qp_count = 0;
  /* How many ways this peer is reached at once. One unless the engine holds
   * a transport per adapter and the peer offers the same, in which case a
   * transfer is split between them. */
  uint32_t lane_count = 1;
  bool remote_device_is_gpu = false;
  uint64_t remote_max_registration_bytes = 0; /* 0 if unbounded. */
};

/* Stable peer handle. Connection identity no longer leaks into every request
 * the way put/get(ip, port, conn_type) did. */
class Peer {
 public:
  virtual ~Peer() = default;

  virtual PeerId id() const = 0;
  virtual Epoch epoch() const = 0;
  virtual PeerCaps caps() const = 0;
  virtual bool connected() const = 0;

  virtual Status import_region(std::vector<uint8_t> const& descriptor,
                               RemoteRegionPtr* out) = 0;
  virtual Status import_region_batch(
      std::vector<std::vector<uint8_t>> const& descriptors,
      std::vector<RemoteRegionPtr>* out) = 0;
};

using PeerPtr = std::shared_ptr<Peer>;

}  // namespace hux
#endif  // HUX_PEER_H
