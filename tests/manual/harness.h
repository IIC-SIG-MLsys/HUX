/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The parts every manual test needs: a socket to exchange a few blobs over,
 * a buffer that is host or device memory depending on the vendor built in,
 * and a way to push a request to its terminal state under explicit progress.
 *
 * These were written three times before this file existed, and the cost of
 * that has already been paid: TCP_NODELAY was added to one copy and not the
 * others, so one test measured Nagle's 40 ms per direction as if it were
 * transfer time. */
#ifndef HUX_TESTS_MANUAL_HARNESS_H
#define HUX_TESTS_MANUAL_HARNESS_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hux/engine.h"

#ifdef HUX_LOOPBACK_CUDA
#include <cuda_runtime.h>

#include "device/cuda_backend.h"
#endif
#ifdef HUX_LOOPBACK_ROCM
#include <hip/hip_runtime.h>

#include "device/rocm_backend.h"
#endif
#ifdef HUX_LOOPBACK_NEUWARE
#include <cnrt.h>

#include "device/neuware_backend.h"
#endif

#ifdef HUX_LOOPBACK_KUNLUN
/* The SDK's own CUDA-compatible runtime. */
#include <cuda_runtime.h>

#include "device/kunlun_backend.h"
#endif

namespace hux {
namespace manual {

/* Small request-and-reply traffic meets Nagle on one side and the delayed
 * acknowledgement on the other, which costs about 40 ms per direction. Not
 * inherited from a listening socket, so it is set on accepted ones too. */
inline void nodelay(int fd) {
  int on = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

/* Checked, because an unchecked bind is how a second server silently hands
 * its clients to the first one still holding the port. */
inline int listen_on(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = INADDR_ANY;
  a.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
      ::listen(fd, 4) != 0) {
    std::printf("port %u is already taken\n", port);
    ::close(fd);
    return -1;
  }
  return fd;
}

inline int accept_one(int srv) {
  int fd = ::accept(srv, nullptr, nullptr);
  if (fd >= 0) nodelay(fd);
  return fd;
}

/* Returns -1 rather than running on with a socket that never connected:
 * reading a handshake off one of those used to crash on an empty buffer,
 * which says nothing about the server that is missing. */
inline int dial(std::string const& ip, uint16_t port, int seconds) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  ::inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
  for (int i = 0; i < seconds * 5; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
      nodelay(fd);
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  ::close(fd);
  return -1;
}

inline bool send_blob(int fd, void const* p, uint32_t n) {
  if (::send(fd, &n, 4, 0) != 4) return false;
  return n == 0 || ::send(fd, p, n, 0) == static_cast<ssize_t>(n);
}

inline bool recv_blob(int fd, std::vector<uint8_t>* out) {
  uint32_t n = 0;
  if (::recv(fd, &n, 4, MSG_WAITALL) != 4) return false;
  out->resize(n);
  return n == 0 ||
         ::recv(fd, out->data(), n, MSG_WAITALL) == static_cast<ssize_t>(n);
}

/* Host memory, or a vendor's device memory when one was built in. The
 * patterns are built once and kept: a soak run rebuilding them per round
 * would put the test's own arithmetic on the critical path. */
struct Buffer {
  std::shared_ptr<DeviceBackend> dev;
  void* ptr = nullptr;
  uint64_t bytes = 0;
  bool on_device = false;
  std::vector<uint8_t> host_backing;

  bool make(int gpu, uint64_t n) {
    bytes = n;
    if (gpu < 0) {
      host_backing.assign(n, 0);
      ptr = host_backing.data();
      on_device = false;
      return true;
    }
#if defined(HUX_LOOPBACK_CUDA)
    if (CudaBackend::create(gpu, &dev) != Status::kOk) return false;
    if (cudaSetDevice(gpu) != cudaSuccess || cudaMalloc(&ptr, n) != cudaSuccess)
      return false;
#elif defined(HUX_LOOPBACK_ROCM)
    if (RocmBackend::create(gpu, &dev) != Status::kOk) return false;
    if (hipSetDevice(gpu) != hipSuccess || hipMalloc(&ptr, n) != hipSuccess)
      return false;
#elif defined(HUX_LOOPBACK_NEUWARE)
    if (NeuwareBackend::create(gpu, &dev) != Status::kOk) return false;
    if (cnrtSetDevice(gpu) != cnrtSuccess || cnrtMalloc(&ptr, n) != cnrtSuccess)
      return false;
#elif defined(HUX_LOOPBACK_KUNLUN)
    if (KunlunBackend::create(gpu, &dev) != Status::kOk) return false;
    if (cudaSetDevice(gpu) != cudaSuccess || cudaMalloc(&ptr, n) != cudaSuccess)
      return false;
#else
    std::printf("built without a device backend\n");
    return false;
#endif
    on_device = dev != nullptr;
    return on_device;
  }

  std::vector<uint8_t> const& pattern(uint8_t seed) const {
    auto it = patterns.find(seed);
    if (it != patterns.end()) return it->second;
    std::vector<uint8_t> v(bytes);
    for (uint64_t i = 0; i < bytes; ++i)
      v[i] = static_cast<uint8_t>(i * 31 + seed);
    return patterns.emplace(seed, std::move(v)).first->second;
  }

  void fill(uint8_t seed) const {
    if (dev != nullptr)
      dev->copy(ptr, pattern(seed).data(), bytes);
    else
      std::memcpy(ptr, pattern(seed).data(), static_cast<size_t>(bytes));
  }

  void read_back(std::vector<uint8_t>* out) const {
    out->resize(bytes);
    if (dev != nullptr)
      dev->copy(out->data(), ptr, bytes);
    else
      std::memcpy(out->data(), ptr, static_cast<size_t>(bytes));
  }

  bool verify(uint8_t seed) const { return first_mismatch(seed) < 0; }

  /* memcmp first: the byte-by-byte search is far slower, and only a round
   * that already failed should pay for finding out where. */
  int64_t first_mismatch(uint8_t seed) const {
    read_back(&scratch);
    auto const& want = pattern(seed);
    if (std::memcmp(scratch.data(), want.data(), static_cast<size_t>(bytes)) ==
        0)
      return -1;
    for (uint64_t i = 0; i < bytes; ++i)
      if (scratch[i] != want[i]) return static_cast<int64_t>(i);
    return -1;
  }

  mutable std::map<uint8_t, std::vector<uint8_t>> patterns;
  mutable std::vector<uint8_t> scratch;
};

/* Progress is explicit in these tests, so completions are collected here
 * rather than by a thread the test does not own. */
inline Status drive(Engine* engine, RequestPtr const& req,
                    int max_polls = 200000) {
  std::vector<RequestPtr> done;
  for (int i = 0; i < max_polls; ++i) {
    engine->poll_completions(32, &done);
    bool fin = false;
    req->test(&fin);
    if (fin) return req->wait(5000);
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  return Status::kTimeout;
}

}  // namespace manual
}  // namespace hux
#endif  // HUX_TESTS_MANUAL_HARNESS_H
