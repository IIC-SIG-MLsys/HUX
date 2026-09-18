/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Wire format for engine-level control messages.
 *
 * Framed explicitly with a length, and every field written at a fixed width
 * and byte order. A raw struct on the wire would depend on the local ABI, so
 * two builds could disagree about padding while both believing they agreed.
 *
 * Control traffic travels on its own channel rather than sharing the data
 * path's budget: a stalled transfer must not be able to starve the message
 * that would explain it. */
#ifndef HUX_CONTROL_MESSAGE_H
#define HUX_CONTROL_MESSAGE_H

#include <cstdint>
#include <vector>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

constexpr uint16_t kControlMajor = 1;
constexpr uint16_t kControlMinor = 0;

enum class ControlType : uint16_t {
  kInvalid = 0,
  /* A write landed: names the request and the region and span it covers, so
   * the target owner can build a dependency on exactly those bytes. The
   * 32-bit immediate that signals arrival cannot carry this. */
  kReadyHandoff = 1,
  /* Application payload, independent of any transfer. */
  kNotification = 2,
  /* A region is no longer valid; the peer must stop submitting against it. */
  kRegionInvalidate = 3,
  /* Confirms a notification reached the peer's queue. Sending succeeds as
   * soon as the bytes leave this host, which says nothing about the peer
   * having accepted them, so the acknowledgement is what the sender's request
   * actually waits on. */
  kNotificationAck = 4,
};

/* Body of a kNotification: an id the sender chose, followed by its payload.
 * The id comes back in the acknowledgement. */
struct NotificationHeader {
  uint64_t id = 0;
};
constexpr size_t kNotificationHeaderBytes = 8;

void encode_u64(uint64_t v, std::vector<uint8_t>* out);
Status decode_u64(std::vector<uint8_t> const& in, uint64_t* out);

struct ControlHeader {
  uint16_t major = kControlMajor;
  uint16_t minor = kControlMinor;
  ControlType type = ControlType::kInvalid;
  uint16_t flags = 0;
  uint32_t payload_len = 0;
};

constexpr size_t kControlHeaderBytes = 2 + 2 + 2 + 2 + 4;

/* Largest payload accepted from a peer. A length field is attacker-controlled
 * in the general case and a careless reader would allocate whatever it says. */
constexpr uint32_t kControlMaxPayload = 64u << 10;

void encode_control_header(ControlHeader const& h, uint8_t* out);
Status decode_control_header(uint8_t const* in, ControlHeader* out);

/* Body of a kReadyHandoff. */
struct ReadyHandoffBody {
  uint64_t request = 0;
  uint64_t region = 0;
  uint32_t generation = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
};

constexpr size_t kReadyHandoffBytes = 8 + 8 + 4 + 8 + 8;

void encode_ready_handoff(ReadyHandoffBody const& b, std::vector<uint8_t>* out);
Status decode_ready_handoff(std::vector<uint8_t> const& in,
                            ReadyHandoffBody* out);

}  // namespace hux
#endif  // HUX_CONTROL_MESSAGE_H
