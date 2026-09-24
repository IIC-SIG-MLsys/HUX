/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "hux/host_memory.h"

#include <sys/mman.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace hux {
namespace {

constexpr uint64_t kHugePage = 2ull << 20;

/* AnonHugePages of the mapping that starts at base, from /proc. The mapping
 * is this allocation's own, so the figure is exactly its own. */
uint64_t huge_bytes_of(void* base) {
  FILE* f = std::fopen("/proc/self/smaps", "r");
  if (f == nullptr) return 0;
  char line[512];
  bool in = false;
  uint64_t kib = 0;
  auto const want = reinterpret_cast<uintptr_t>(base);
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    uintptr_t lo = 0, hi = 0;
    /* A mapping's header is "lo-hi perms ..."; its fields are "Name: n kB",
     * which never scan as two addresses joined by a dash. */
    if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " ", &lo, &hi) == 2) {
      if (in) break; /* past ours */
      in = lo == want;
      continue;
    }
    if (in && std::sscanf(line, "AnonHugePages: %" SCNu64 " kB", &kib) == 1)
      break;
  }
  std::fclose(f);
  return kib << 10;
}

}  // namespace

Status alloc_host(uint64_t bytes, HostAllocation* out) {
  if (out == nullptr || bytes == 0) return Status::kInvalidArgument;
  *out = HostAllocation{};
  uint64_t const len = (bytes + kHugePage - 1) / kHugePage * kHugePage;
  /* Over-allocated by a page and trimmed, since mmap promises only page
   * alignment and a huge page needs the address aligned to its size. */
  void* raw = ::mmap(nullptr, len + kHugePage, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) return Status::kResourceExhausted;
  auto const r = reinterpret_cast<uintptr_t>(raw);
  uintptr_t const base = (r + kHugePage - 1) / kHugePage * kHugePage;
  if (base > r) ::munmap(raw, base - r);
  uintptr_t const end = base + len;
  if (r + len + kHugePage > end)
    ::munmap(reinterpret_cast<void*>(end), r + len + kHugePage - end);
  void* p = reinterpret_cast<void*>(base);
  /* Before the first touch: pages faulted in small stay small until
   * khugepaged gets to them, and a registration pins them before it does. */
  ::madvise(p, len, MADV_HUGEPAGE);
  std::memset(p, 0, len);
  out->ptr = p;
  out->bytes = len;
  out->huge_bytes = huge_bytes_of(p);
  return Status::kOk;
}

void free_host(HostAllocation const& a) {
  if (a.ptr != nullptr && a.bytes > 0) ::munmap(a.ptr, a.bytes);
}

}  // namespace hux
