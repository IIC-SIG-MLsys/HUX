/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Version rules for the connection handshake, checkable without two hosts. */
#include "test_main.h"
#include "transport/rdma/rdma_provider.h"

using namespace hux;

HUX_TEST(wire_version_accepts_same_major) {
  CHECK_STATUS(check_wire_version(kWireMajor, kWireMinor), Status::kOk);
}

HUX_TEST(wire_version_accepts_newer_minor) {
  /* Minor changes only add fields an older end can ignore, so a peer ahead on
   * minor stays compatible. */
  CHECK_STATUS(check_wire_version(kWireMajor, kWireMinor + 5), Status::kOk);
}

HUX_TEST(wire_version_refuses_any_major_mismatch) {
  /* In both directions: being the newer end is no reason to accept a layout
   * the peer does not share. */
  CHECK_STATUS(check_wire_version(kWireMajor + 1, 0), Status::kUnsupported);
  if (kWireMajor > 0)
    CHECK_STATUS(check_wire_version(kWireMajor - 1, 0), Status::kUnsupported);
}
