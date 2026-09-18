/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "control/identity.h"

#include <unistd.h>

#include <atomic>
#include <fstream>
#include <functional>

namespace hux {
namespace {

/* A stable machine identifier. /etc/machine-id survives reboots and differs
 * between containers sharing a kernel; boot_id is the fallback because it at
 * least differs between machines. A hostname is not used: containers
 * routinely share one, and two engines would then believe they could address
 * each other's memory. */
uint64_t read_host_id() {
  for (char const* path :
       {"/etc/machine-id", "/proc/sys/kernel/random/boot_id"}) {
    std::ifstream f(path);
    if (!f) continue;
    std::string line;
    std::getline(f, line);
    if (line.empty()) continue;
    return std::hash<std::string>{}(line);
  }
  /* Nothing readable: a value that is at least unique to this process, so
   * distinct hosts are never mistaken for one. Being wrong in this direction
   * costs a slower path; the other direction corrupts memory. */
  return std::hash<std::string>{}("unknown") ^
         static_cast<uint64_t>(::getpid());
}

void put_u64(std::vector<uint8_t>* o, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    o->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

uint64_t get_u64(uint8_t const* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

}  // namespace

Identity const& local_identity() {
  static Identity const id = [] {
    Identity i;
    i.host = read_host_id();
    i.process = static_cast<uint64_t>(::getpid());
    i.engine = 0;
    return i;
  }();
  return id;
}

uint64_t next_engine_id() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

char const* to_string(Locality l) {
  switch (l) {
    case Locality::kSameEngine:
      return "same_engine";
    case Locality::kSameProcess:
      return "same_process";
    case Locality::kSameHost:
      return "same_host";
    case Locality::kRemote:
      return "remote";
  }
  return "unknown";
}

Locality locality_of(Identity const& a, Identity const& b) {
  if (a.host != b.host) return Locality::kRemote;
  if (a.process != b.process) return Locality::kSameHost;
  if (a.engine != b.engine) return Locality::kSameProcess;
  return Locality::kSameEngine;
}

void encode_identity(Identity const& id, std::vector<uint8_t>* out) {
  put_u64(out, id.host);
  put_u64(out, id.process);
  put_u64(out, id.engine);
}

Status decode_identity(std::vector<uint8_t> const& in, size_t offset,
                       Identity* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  if (in.size() < offset + kIdentityBytes) return Status::kInvalidArgument;
  uint8_t const* p = in.data() + offset;
  out->host = get_u64(p);
  out->process = get_u64(p + 8);
  out->engine = get_u64(p + 16);
  return Status::kOk;
}

}  // namespace hux
