/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Host memory for buffers an adapter moves, on 2 MiB pages where the kernel
 * grants them.
 *
 * Where an IOMMU translates the adapter's accesses -- Grace does, with its
 * SMMU -- every 4 KiB page is a translation, and a large transfer from host
 * memory is bound by that rather than by the link: 4 MiB writes between two
 * GH200s ran at 92 Gb/s from ordinary pages and at 320 Gb/s from these, on
 * the same 400 Gb/s port. A caller that owns its staging buffers should take
 * them from here; one that registers memory from elsewhere is unaffected. */
#ifndef HUX_HOST_MEMORY_H
#define HUX_HOST_MEMORY_H

#include <cstdint>

#include "hux/status.h"

namespace hux {

struct HostAllocation {
  void* ptr = nullptr;
  /* Rounded up to whole 2 MiB pages. */
  uint64_t bytes = 0;
  /* How much of it the kernel actually put on huge pages. Asking is not
   * getting: with transparent huge pages set to "never", or memory too
   * fragmented to find whole pages, this is less than bytes -- the memory
   * still works, only without the benefit. */
  uint64_t huge_bytes = 0;
};

/* Zero-filled and already faulted in, so the pages are in place before a
 * registration pins them. kResourceExhausted if the memory cannot be had. */
Status alloc_host(uint64_t bytes, HostAllocation* out);
/* Releases what alloc_host returned. Any registration of it must have gone
 * first. */
void free_host(HostAllocation const& a);

}  // namespace hux
#endif  // HUX_HOST_MEMORY_H
