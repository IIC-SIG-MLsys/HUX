/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "core/region_impl.h"

#include <cstring>

namespace hux {
namespace {

void put_u16(std::vector<uint8_t>* o, uint16_t v) {
  o->push_back(static_cast<uint8_t>(v & 0xff));
  o->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}
void put_u32(std::vector<uint8_t>* o, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    o->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
void put_u64(std::vector<uint8_t>* o, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    o->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
uint16_t get_u16(uint8_t const* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8;
}
uint32_t get_u32(uint8_t const* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}
uint64_t get_u64(uint8_t const* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

/* major(2) minor(2) region(8) gen(4) base(8) len(8) rkey(8) kind(1) idx(4)
 * access(4) */
constexpr size_t kDescriptorBytes = 2 + 2 + 8 + 4 + 8 + 8 + 8 + 1 + 4 + 4;

}  // namespace

void encode_descriptor(RegionDescriptor const& d, std::vector<uint8_t>* out) {
  out->clear();
  out->reserve(kDescriptorBytes);
  put_u16(out, kDescriptorMajor);
  put_u16(out, kDescriptorMinor);
  put_u64(out, d.region);
  put_u32(out, d.generation);
  put_u64(out, d.base);
  put_u64(out, d.length);
  put_u64(out, d.remote_key);
  out->push_back(static_cast<uint8_t>(d.device_kind));
  put_u32(out, static_cast<uint32_t>(d.device_index));
  put_u32(out, static_cast<uint32_t>(d.access));
}

Status decode_descriptor(std::vector<uint8_t> const& buf,
                         RegionDescriptor* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  if (buf.size() != kDescriptorBytes) return Status::kInvalidArgument;
  uint8_t const* p = buf.data();
  out->major = get_u16(p);
  p += 2;
  out->minor = get_u16(p);
  p += 2;
  /* Reject an incompatible major rather than parsing best-effort. */
  if (out->major != kDescriptorMajor) return Status::kUnsupported;
  out->region = get_u64(p);
  p += 8;
  out->generation = get_u32(p);
  p += 4;
  out->base = get_u64(p);
  p += 8;
  out->length = get_u64(p);
  p += 8;
  out->remote_key = get_u64(p);
  p += 8;
  out->device_kind = static_cast<DeviceKind>(*p);
  p += 1;
  out->device_index = static_cast<int32_t>(get_u32(p));
  p += 4;
  out->access = static_cast<AccessFlags>(get_u32(p));
  return Status::kOk;
}

bool registration_covers(Registration const& r, void* addr, uint64_t length,
                         DeviceId device, AccessFlags access) {
  if (r.device != device) return false;
  /* Every permission asked for must already be held. A read-only registration
   * cannot serve a request that needs to write. */
  uint32_t const want = static_cast<uint32_t>(access);
  uint32_t const have = static_cast<uint32_t>(r.access);
  if ((want & ~have) != 0) return false;

  auto const start = reinterpret_cast<uintptr_t>(addr);
  auto const base = reinterpret_cast<uintptr_t>(r.base);
  if (start < base) return false;
  uint64_t const offset = start - base;
  /* Compared before adding, so a wrapping range cannot appear to fit. */
  return offset <= r.length && length <= r.length - offset;
}

MemoryRegionImpl::MemoryRegionImpl(RegionId id, Generation gen, void* base,
                                   uint64_t length, DeviceId dev,
                                   MemoryKind mem, AccessFlags access,
                                   RegistrationPtr reg)
    : id_(id),
      gen_(gen),
      base_(base),
      length_(length),
      dev_(dev),
      mem_(mem),
      access_(access),
      reg_(std::move(reg)) {}

Status MemoryRegionImpl::view(uint64_t offset, uint64_t length,
                              RegionView* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  Span s{offset, length};
  /* within() compares before adding, so a wrapping range cannot pass. */
  if (!s.within(length_)) return Status::kOutOfRange;
  out->region = id_;
  out->span = s;
  return Status::kOk;
}

Status MemoryRegionImpl::export_descriptor(std::vector<uint8_t>* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  RegionDescriptor d;
  d.region = id_;
  d.generation = gen_;
  d.base = reinterpret_cast<uint64_t>(base_);
  d.length = length_;
  /* Must export the rkey. Substituting the lkey happens to work where the two
   * coincide and breaks silently elsewhere. */
  /* The key belongs to the whole registration, while the address is this
   * handle's own: a view into a pool must advertise where it actually starts,
   * or the peer writes somewhere valid but wrong and nothing reports it. */
  d.remote_key = reg_->remote_key;
  d.device_kind = dev_.kind;
  d.device_index = dev_.index;
  d.access = access_;
  encode_descriptor(d, out);
  return Status::kOk;
}

RemoteRegionImpl::RemoteRegionImpl(RegionDescriptor const& d) : d_(d) {}

DeviceId RemoteRegionImpl::device() const {
  DeviceId id;
  id.kind = d_.device_kind;
  id.index = d_.device_index;
  return id;
}

Status RemoteRegionImpl::view(uint64_t offset, uint64_t length,
                              RegionView* out) const {
  if (out == nullptr) return Status::kInvalidArgument;
  if (!valid()) return Status::kStaleGeneration;
  Span s{offset, length};
  if (!s.within(d_.length)) return Status::kOutOfRange;
  out->region = d_.region;
  out->span = s;
  return Status::kOk;
}

}  // namespace hux
