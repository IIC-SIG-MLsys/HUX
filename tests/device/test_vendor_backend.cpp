/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * One suite, compiled once per vendor. Every backend that claims a capability
 * has to behave the same way here, which is what keeps "supported" from
 * meaning something different on each device.
 *
 * Built with exactly one of HUX_TEST_ROCM / HUX_TEST_NEUWARE / HUX_TEST_MUSA.
 */
#include <memory>

#include "test_main.h"

#if defined(HUX_TEST_ROCM)
#include <hip/hip_runtime.h>

#include "device/rocm_backend.h"
using VendorBackend = hux::RocmBackend;
using VendorStream = hipStream_t;
#define VENDOR_NAME "ROCm/DTK"
#define VENDOR_KIND hux::DeviceKind::kRocm
#define VENDOR_MALLOC(p, n) (hipMalloc((p), (n)) == hipSuccess)
#define VENDOR_FREE(p) ((void)hipFree(p))
#define VENDOR_STREAM_CREATE(s) (hipStreamCreate(s) == hipSuccess)
#define VENDOR_STREAM_DESTROY(s) ((void)hipStreamDestroy(s))
#define VENDOR_STREAM_SYNC(s) (hipStreamSynchronize(s) == hipSuccess)

#elif defined(HUX_TEST_NEUWARE)
#include <cnrt.h>

#include "device/neuware_backend.h"
using VendorBackend = hux::NeuwareBackend;
using VendorStream = cnrtQueue_t;
#define VENDOR_NAME "Cambricon"
#define VENDOR_KIND hux::DeviceKind::kCambricon
#define VENDOR_MALLOC(p, n) (cnrtMalloc((p), (n)) == cnrtSuccess)
#define VENDOR_FREE(p) ((void)cnrtFree(p))
#define VENDOR_STREAM_CREATE(s) (cnrtQueueCreate(s) == cnrtSuccess)
#define VENDOR_STREAM_DESTROY(s) ((void)cnrtQueueDestroy(s))
#define VENDOR_STREAM_SYNC(s) (cnrtQueueSync(s) == cnrtSuccess)

#elif defined(HUX_TEST_MUSA)
#include <musa_runtime.h>

#include "device/musa_backend.h"
using VendorBackend = hux::MusaBackend;
using VendorStream = musaStream_t;
#define VENDOR_NAME "Moore Threads"
#define VENDOR_KIND hux::DeviceKind::kMoore
#define VENDOR_MALLOC(p, n) (musaMalloc((p), (n)) == musaSuccess)
#define VENDOR_FREE(p) ((void)musaFree(p))
#define VENDOR_STREAM_CREATE(s) (musaStreamCreate(s) == musaSuccess)
#define VENDOR_STREAM_DESTROY(s) ((void)musaStreamDestroy(s))
#define VENDOR_STREAM_SYNC(s) (musaStreamSynchronize(s) == musaSuccess)

#elif defined(HUX_TEST_KUNLUN)
/* The SDK's own CUDA-compatible runtime, not a real CUDA one. */
#include <cuda_runtime.h>

#include "device/kunlun_backend.h"
using VendorBackend = hux::KunlunBackend;
using VendorStream = cudaStream_t;
#define VENDOR_NAME "Kunlunxin"
#define VENDOR_KIND hux::DeviceKind::kKunlun
#define VENDOR_MALLOC(p, n) (cudaMalloc((p), (n)) == cudaSuccess)
#define VENDOR_FREE(p) ((void)cudaFree(p))
#define VENDOR_STREAM_CREATE(s) (cudaStreamCreate(s) == cudaSuccess)
#define VENDOR_STREAM_DESTROY(s) ((void)cudaStreamDestroy(s))
#define VENDOR_STREAM_SYNC(s) (cudaStreamSynchronize(s) == cudaSuccess)

#else
#error "Define one of HUX_TEST_ROCM / _NEUWARE / _MUSA / _KUNLUN"
#endif

using namespace hux;

namespace {

std::shared_ptr<DeviceBackend> make_backend() {
  std::shared_ptr<DeviceBackend> b;
  if (VendorBackend::create(0, &b) != Status::kOk) return nullptr;
  return b;
}

}  // namespace

HUX_TEST(vendor_backend_reports_its_kind) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no " VENDOR_NAME " device");
  CHECK(b->kind() == VENDOR_KIND);
}

HUX_TEST(vendor_backend_probes_device_pointer) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no " VENDOR_NAME " device");

  void* dptr = nullptr;
  CHECK(VENDOR_MALLOC(&dptr, 4096));

  DeviceId dev;
  MemoryKind mem;
  CHECK_STATUS(b->probe_pointer(dptr, &dev, &mem), Status::kOk);
  /* The device a pointer belongs to comes from the runtime, never from what
   * the caller assumed when allocating it. */
  CHECK(dev.kind == VENDOR_KIND);
  CHECK(mem == MemoryKind::kDevice);

  VENDOR_FREE(dptr);
}

HUX_TEST(vendor_backend_probes_pageable_host_pointer) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no " VENDOR_NAME " device");

  int on_stack = 0;
  DeviceId dev;
  MemoryKind mem;
  CHECK_STATUS(b->probe_pointer(&on_stack, &dev, &mem), Status::kOk);
  CHECK(dev.kind == DeviceKind::kHost);
  CHECK(mem == MemoryKind::kHostPageable);
}

HUX_TEST(vendor_backend_records_and_waits_on_event) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no " VENDOR_NAME " device");
  if (!b->caps().supports_stream) SKIP("backend reports no stream support");

  VendorStream raw{};
  CHECK(VENDOR_STREAM_CREATE(&raw));

  DeviceStreamPtr s;
  CHECK_STATUS(b->import_stream(raw, &s), Status::kOk);
  CHECK(s->device().kind == VENDOR_KIND);

  DeviceEventPtr e;
  CHECK_STATUS(b->record_event(s.get(), &e), Status::kOk);
  CHECK_EQ(e->recorded(), true);

  CHECK_STATUS(b->stream_wait_event(s.get(), e.get()), Status::kOk);
  CHECK(VENDOR_STREAM_SYNC(raw));

  bool complete = false;
  CHECK_STATUS(e->query(&complete), Status::kOk);
  CHECK_EQ(complete, true);

  VENDOR_STREAM_DESTROY(raw);
}

HUX_TEST(vendor_backend_rejects_null_arguments) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no " VENDOR_NAME " device");

  DeviceId dev;
  MemoryKind mem;
  CHECK_STATUS(b->probe_pointer(nullptr, &dev, &mem), Status::kInvalidArgument);
  CHECK_STATUS(b->stream_wait_event(nullptr, nullptr),
               Status::kInvalidArgument);
  DeviceEventPtr e;
  CHECK_STATUS(b->record_event(nullptr, &e), Status::kInvalidArgument);
}

HUX_TEST(vendor_backend_rejects_invalid_device_index) {
  if (make_backend() == nullptr) SKIP("no " VENDOR_NAME " device");
  std::shared_ptr<DeviceBackend> b;
  CHECK_STATUS(VendorBackend::create(9999, &b), Status::kInvalidArgument);
}

HUX_TEST(vendor_backend_states_registration_limits) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no " VENDOR_NAME " device");
  DeviceCaps c = b->caps();

  /* A per-call limit without a total is fine, and so is neither, but a total
   * smaller than one allowed call would be incoherent. */
  if (c.max_total_registration_bytes != 0 && c.max_registration_bytes != 0) {
    CHECK(c.max_registration_bytes <= c.max_total_registration_bytes);
  }

  std::printf(
      "       %s: peer_registration=%s single=%llu MiB total=%llu MiB\n",
      VENDOR_NAME, c.supports_peer_registration ? "yes" : "no",
      (unsigned long long)(c.max_registration_bytes >> 20),
      (unsigned long long)(c.max_total_registration_bytes >> 20));
}
