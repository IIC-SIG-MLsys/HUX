/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * A backend must state what it cannot do. These cases pin down that the host
 * backend reports kUnsupported for stream work instead of quietly returning
 * success, which would let a caller build a dependency that orders nothing. */
#include <vector>

#include "device/host_backend.h"
#include "test_main.h"

using namespace hux;

HUX_TEST(host_backend_reports_no_stream_support) {
  HostBackend b;
  CHECK(b.kind() == DeviceKind::kHost);
  DeviceCaps c = b.caps();
  CHECK_EQ(c.supports_stream, false);
  CHECK_EQ(c.supports_graph_capture, false);
  CHECK_EQ(c.max_registration_bytes, 0u);
}

HUX_TEST(host_backend_stream_calls_are_unsupported_not_ok) {
  HostBackend b;
  DeviceStreamPtr s;
  DeviceEventPtr e;
  CHECK_STATUS(b.import_stream(nullptr, &s), Status::kUnsupported);
  CHECK_STATUS(b.record_event(nullptr, &e), Status::kUnsupported);
  CHECK_STATUS(b.stream_wait_event(nullptr, nullptr), Status::kUnsupported);
}

HUX_TEST(host_backend_probes_host_pointer) {
  HostBackend b;
  std::vector<uint8_t> buf(64, 0);
  DeviceId dev;
  MemoryKind mem;
  CHECK_STATUS(b.probe_pointer(buf.data(), &dev, &mem), Status::kOk);
  CHECK(dev.kind == DeviceKind::kHost);
  CHECK(mem == MemoryKind::kHostPageable);
}

HUX_TEST(host_backend_rejects_null_probe) {
  HostBackend b;
  DeviceId dev;
  MemoryKind mem;
  CHECK_STATUS(b.probe_pointer(nullptr, &dev, &mem), Status::kInvalidArgument);
}
