/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Who a peer is, in the terms that decide which path reaches it.
 *
 * Deliberately not an IP address or a rank. Two ranks can share a host, one
 * host can have several addresses, and neither tells you whether a pointer on
 * the far side is addressable here. Host identity and process identity do. */
#ifndef HUX_CONTROL_IDENTITY_H
#define HUX_CONTROL_IDENTITY_H

#include <cstdint>
#include <string>
#include <vector>

#include "hux/status.h"

namespace hux {

struct Identity {
  /* Stable for the life of the machine. Taken from the kernel rather than
   * from a hostname, which can be duplicated across containers. */
  uint64_t host = 0;
  uint64_t process = 0;
  /* Distinguishes engines within one process, so a program can run several
   * without them mistaking each other for themselves. */
  uint64_t engine = 0;
};

/* This process's identity, computed once. */
Identity const& local_identity();

/* A new engine identity, unique within this process. */
uint64_t next_engine_id();

enum class Locality : uint8_t {
  kSameEngine = 0, /* the same engine: nothing to transfer between */
  kSameProcess,    /* addressable directly */
  kSameHost,       /* shared memory or IPC, but not one address space */
  kRemote,
};

char const* to_string(Locality l);
Locality locality_of(Identity const& a, Identity const& b);

void encode_identity(Identity const& id, std::vector<uint8_t>* out);
Status decode_identity(std::vector<uint8_t> const& in, size_t offset,
                       Identity* out);
constexpr size_t kIdentityBytes = 24;

}  // namespace hux
#endif  // HUX_CONTROL_IDENTITY_H
