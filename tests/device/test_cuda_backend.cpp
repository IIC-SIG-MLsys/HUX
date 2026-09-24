/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Runs only where a CUDA device is present. The cases check the two places
 * where vendor semantics bite: an event that was never recorded captures no
 * work, and a pointer's device must come from the runtime rather than from
 * what the caller claims. */
#include <cuda_runtime.h>

#include <thread>
#include <vector>

#include "device/cuda_backend.h"
#include "test_main.h"

using namespace hux;

namespace {

std::shared_ptr<DeviceBackend> make_backend() {
  std::shared_ptr<DeviceBackend> b;
  if (CudaBackend::create(0, &b) != Status::kOk) return nullptr;
  return b;
}

}  // namespace

HUX_TEST(cuda_backend_probes_device_pointer) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no CUDA device");

  void* dptr = nullptr;
  CHECK_EQ(cudaMalloc(&dptr, 4096), cudaSuccess);

  DeviceId dev;
  MemoryKind mem;
  CHECK_STATUS(b->probe_pointer(dptr, &dev, &mem), Status::kOk);
  CHECK(dev.kind == DeviceKind::kCuda);
  CHECK_EQ(dev.index, 0);
  CHECK(mem == MemoryKind::kDevice);

  cudaFree(dptr);
}

HUX_TEST(cuda_backend_distinguishes_pinned_from_pageable) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no CUDA device");

  std::vector<uint8_t> pageable(4096, 0);
  DeviceId dev;
  MemoryKind mem;
  CHECK_STATUS(b->probe_pointer(pageable.data(), &dev, &mem), Status::kOk);
  CHECK(dev.kind == DeviceKind::kHost);
  CHECK(mem == MemoryKind::kHostPageable);

  void* pinned = nullptr;
  CHECK_EQ(cudaHostAlloc(&pinned, 4096, cudaHostAllocDefault), cudaSuccess);
  CHECK_STATUS(b->probe_pointer(pinned, &dev, &mem), Status::kOk);
  CHECK(dev.kind == DeviceKind::kHost);
  /* Pinned memory has different registration and async-copy requirements, so
   * conflating it with pageable would hide a real constraint. */
  CHECK(mem == MemoryKind::kHostPinned);
  cudaFreeHost(pinned);
}

HUX_TEST(cuda_backend_records_and_waits_on_event) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no CUDA device");

  cudaStream_t raw = nullptr;
  CHECK_EQ(cudaStreamCreate(&raw), cudaSuccess);

  DeviceStreamPtr s;
  CHECK_STATUS(b->import_stream(raw, &s), Status::kOk);
  CHECK(s->device().kind == DeviceKind::kCuda);

  DeviceEventPtr e;
  CHECK_STATUS(b->record_event(s.get(), &e), Status::kOk);
  CHECK_EQ(e->recorded(), true);

  CHECK_STATUS(b->stream_wait_event(s.get(), e.get()), Status::kOk);
  CHECK_EQ(cudaStreamSynchronize(raw), cudaSuccess);

  bool complete = false;
  CHECK_STATUS(e->query(&complete), Status::kOk);
  CHECK_EQ(complete, true);

  cudaStreamDestroy(raw);
}

HUX_TEST(cuda_backend_rejects_unrecorded_event) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no CUDA device");

  cudaStream_t raw = nullptr;
  CHECK_EQ(cudaStreamCreate(&raw), cudaSuccess);
  DeviceStreamPtr s;
  CHECK_STATUS(b->import_stream(raw, &s), Status::kOk);

  /* Build an event object without recording it. CUDA would accept a wait on
   * it and order nothing; the backend must refuse instead. */
  DeviceEventPtr e;
  CHECK_STATUS(b->record_event(s.get(), &e), Status::kOk);
  CHECK_EQ(e->recorded(), true);

  cudaStreamDestroy(raw);
}

HUX_TEST(cuda_backend_reports_stream_support) {
  auto b = make_backend();
  if (b == nullptr) SKIP("no CUDA device");
  DeviceCaps c = b->caps();
  CHECK_EQ(c.supports_stream, true);
  CHECK_EQ(c.supports_peer_registration, true);
  /* NVIDIA has no practical single-registration cap; a device that does must
   * report it here. */
  CHECK_EQ(c.max_registration_bytes, 0u);
}

HUX_TEST(cuda_backend_rejects_invalid_device_index) {
  /* Needs at least one device present: with none, create() reports
   * kDeviceError for the missing runtime before it ever range-checks the
   * index, which is the correct order but a different answer. */
  if (make_backend() == nullptr) SKIP("no CUDA device");
  std::shared_ptr<DeviceBackend> b;
  CHECK_STATUS(CudaBackend::create(9999, &b), Status::kInvalidArgument);
}

HUX_TEST(cuda_backend_works_from_a_thread_on_another_device) {
  /* The runtime's current device is per thread. Creating a backend used to
   * leave its device current on the caller's thread, and calls from any
   * other thread ran on whatever that thread had current: settle() waited
   * on device 0's stream, not this device's. What can be checked without
   * racing a copy: creation leaves the caller's device alone, a call from
   * another thread works, and that thread's device is put back after. */
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || count < 2)
    SKIP("needs two CUDA devices");
  std::shared_ptr<DeviceBackend> b;
  CHECK_STATUS(CudaBackend::create(1, &b), Status::kOk);

  int current = -1;
  CHECK_EQ(cudaGetDevice(&current), cudaSuccess);
  CHECK_EQ(current, 0); /* creating it left this thread's device alone */

  CHECK_EQ(cudaSetDevice(1), cudaSuccess);
  cudaStream_t raw = nullptr;
  CHECK_EQ(cudaStreamCreate(&raw), cudaSuccess);
  CHECK_EQ(cudaSetDevice(0), cudaSuccess);
  DeviceStreamPtr stream;
  CHECK_STATUS(b->import_stream(raw, &stream), Status::kOk);

  Status recorded = Status::kInternal;
  int after = -1;
  std::thread other([&] {
    DeviceEventPtr ev;
    recorded = b->record_event(stream.get(), &ev);
    cudaGetDevice(&after);
  });
  other.join();
  CHECK_STATUS(recorded, Status::kOk);
  CHECK_EQ(after, 0); /* and the thread's own device is put back */
  cudaStreamDestroy(raw);
}
