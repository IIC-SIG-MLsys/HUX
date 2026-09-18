/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Answers one question about a new accelerator: can its device memory be
 * registered for RDMA, and up to what size? Everything else about a backend
 * follows from that answer, and reading vendor code does not give it -- on
 * Moore Threads S3000 device memory fails at every size while pinned host
 * memory succeeds, and on Cambricon MLU registration works but only up to
 * roughly 32 MiB.
 *
 * Build for the vendor under test, e.g.
 *   g++ -std=c++17 probe_registration.cpp -o probe -DHUX_PROBE_MUSA \
 *       -I$MUSA_HOME/include -L$MUSA_LIB -lmusart -lmusa -libverbs
 */
#include <infiniband/verbs.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(HUX_PROBE_CUDA)
#include <cuda_runtime.h>
#elif defined(HUX_PROBE_MUSA)
#include <musa_runtime.h>
#elif defined(HUX_PROBE_ROCM)
#include <hip/hip_runtime.h>
#elif defined(HUX_PROBE_NEUWARE)
/* Both paths are probed: HMC allocates through the driver API while UCCL uses
 * the runtime one, and the two have differed before. */
#include <cn_api.h>
#include <cnrt.h>
#endif

namespace {

constexpr int kAccess = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                        IBV_ACCESS_REMOTE_WRITE;

/* Sizes chosen to bracket the limits seen so far: a page, then powers up past
 * the Cambricon ceiling. */
constexpr size_t kSizes[] = {4096,        1u << 20,   4u << 20,
                             32u << 20,   128u << 20, 512u << 20};

bool try_register(ibv_pd* pd, void* ptr, size_t bytes, char* out, size_t n) {
  errno = 0;
  ibv_mr* mr = ibv_reg_mr(pd, ptr, bytes, kAccess);
  if (mr != nullptr) {
    std::snprintf(out, n, "OK");
    ibv_dereg_mr(mr);
    return true;
  }
  std::snprintf(out, n, "FAIL %s", std::strerror(errno));
  return false;
}

}  // namespace

int main() {
  int num = 0;
  ibv_device** list = ibv_get_device_list(&num);
  if (list == nullptr || num == 0) {
    std::printf("no RDMA device found\n");
    return 1;
  }
  ibv_context* ctx = ibv_open_device(list[0]);
  if (ctx == nullptr) {
    std::printf("ibv_open_device failed\n");
    return 1;
  }
  ibv_pd* pd = ibv_alloc_pd(ctx);
  if (pd == nullptr) {
    std::printf("ibv_alloc_pd failed\n");
    return 1;
  }
  std::printf("NIC: %s\n\n", ibv_get_device_name(list[0]));
  std::printf("%-12s %-26s %s\n", "size", "device memory", "pinned host memory");

  for (size_t s : kSizes) {
    char dev_res[96], host_res[96];
    std::snprintf(dev_res, sizeof dev_res, "n/a (no vendor SDK)");
    std::snprintf(host_res, sizeof host_res, "n/a");

#if defined(HUX_PROBE_CUDA)
    void* dptr = nullptr;
    if (cudaMalloc(&dptr, s) == cudaSuccess) {
      try_register(pd, dptr, s, dev_res, sizeof dev_res);
      cudaFree(dptr);
    } else {
      std::snprintf(dev_res, sizeof dev_res, "alloc failed");
    }
    void* hptr = nullptr;
    if (cudaHostAlloc(&hptr, s, cudaHostAllocDefault) == cudaSuccess) {
      try_register(pd, hptr, s, host_res, sizeof host_res);
      cudaFreeHost(hptr);
    }
#elif defined(HUX_PROBE_MUSA)
    void* dptr = nullptr;
    if (musaMalloc(&dptr, s) == musaSuccess) {
      try_register(pd, dptr, s, dev_res, sizeof dev_res);
      musaFree(dptr);
    } else {
      std::snprintf(dev_res, sizeof dev_res, "alloc failed");
    }
    void* hptr = nullptr;
    if (posix_memalign(&hptr, 128, s) == 0 && hptr != nullptr) {
      if (musaHostRegister(hptr, s,
                           musaHostRegisterMapped | musaHostRegisterPortable) ==
          musaSuccess) {
        try_register(pd, hptr, s, host_res, sizeof host_res);
        musaHostUnregister(hptr);
      } else {
        std::snprintf(host_res, sizeof host_res, "host register failed");
      }
      std::free(hptr);
    }
#elif defined(HUX_PROBE_NEUWARE)
    void* dptr = nullptr;
    if (cnrtMalloc(&dptr, s) == cnrtSuccess) {
      try_register(pd, dptr, s, dev_res, sizeof dev_res);
      cnrtFree(dptr);
    } else {
      std::snprintf(dev_res, sizeof dev_res, "alloc failed");
    }
    /* cnMallocPeerAble looks like the right call for peer access, but its
     * memory is rejected by ibv_reg_mr at every size on Neuware v6, so the
     * plain allocation above is what a backend must use. */
    CNaddr paddr = 0;
    if (cnMallocPeerAble(&paddr, s) == CN_SUCCESS) {
      char peer_res[96];
      try_register(pd, reinterpret_cast<void*>(paddr), s, peer_res,
                   sizeof peer_res);
      std::snprintf(host_res, sizeof host_res, "peerAble: %s", peer_res);
      cnFree(paddr);
    } else {
      std::snprintf(host_res, sizeof host_res, "peerAble: alloc failed");
    }
#elif defined(HUX_PROBE_ROCM)
    void* dptr = nullptr;
    if (hipMalloc(&dptr, s) == hipSuccess) {
      try_register(pd, dptr, s, dev_res, sizeof dev_res);
      hipFree(dptr);
    } else {
      std::snprintf(dev_res, sizeof dev_res, "alloc failed");
    }
    void* hptr = nullptr;
    if (hipHostMalloc(&hptr, s, hipHostMallocDefault) == hipSuccess) {
      try_register(pd, hptr, s, host_res, sizeof host_res);
      hipHostFree(hptr);
    }
#endif
    std::printf("%-12zu %-26s %s\n", s, dev_res, host_res);
  }

  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(list);
  return 0;
}
