/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "control/control_message.h"

namespace hux {
namespace {

void put_u16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}
void put_u32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
}
void put_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
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

}  // namespace

void encode_control_header(ControlHeader const& h, uint8_t* out) {
  put_u16(out, h.major);
  put_u16(out + 2, h.minor);
  put_u16(out + 4, static_cast<uint16_t>(h.type));
  put_u16(out + 6, h.flags);
  put_u32(out + 8, h.payload_len);
}

Status decode_control_header(uint8_t const* in, ControlHeader* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  out->major = get_u16(in);
  out->minor = get_u16(in + 2);
  out->type = static_cast<ControlType>(get_u16(in + 4));
  out->flags = get_u16(in + 6);
  out->payload_len = get_u32(in + 8);

  /* Refuse a major mismatch rather than parsing a layout only one end
   * understands. */
  if (out->major != kControlMajor) return Status::kUnsupported;
  /* A peer could claim any length; believing it would mean allocating
   * whatever it asked for. */
  if (out->payload_len > kControlMaxPayload) return Status::kInvalidArgument;
  return Status::kOk;
}

void encode_u64(uint64_t v, std::vector<uint8_t>* out) {
  out->assign(8, 0);
  put_u64(out->data(), v);
}

Status decode_u64(std::vector<uint8_t> const& in, uint64_t* out) {
  if (out == nullptr || in.size() < 8) return Status::kInvalidArgument;
  *out = get_u64(in.data());
  return Status::kOk;
}

void encode_region_invalidate(RegionInvalidateBody const& b,
                              std::vector<uint8_t>* out) {
  out->assign(kRegionInvalidateBytes, 0);
  put_u64(out->data(), b.region);
  put_u32(out->data() + 8, b.generation);
}

Status decode_region_invalidate(std::vector<uint8_t> const& in,
                                RegionInvalidateBody* out) {
  if (out == nullptr || in.size() != kRegionInvalidateBytes)
    return Status::kInvalidArgument;
  out->region = get_u64(in.data());
  out->generation = get_u32(in.data() + 8);
  return Status::kOk;
}

void encode_ready_handoff(ReadyHandoffBody const& b,
                          std::vector<uint8_t>* out) {
  out->assign(kReadyHandoffBytes, 0);
  uint8_t* p = out->data();
  put_u64(p, b.request);
  put_u64(p + 8, b.region);
  put_u32(p + 16, b.generation);
  put_u64(p + 20, b.offset);
  put_u64(p + 28, b.length);
}

Status decode_ready_handoff(std::vector<uint8_t> const& in,
                            ReadyHandoffBody* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  if (in.size() != kReadyHandoffBytes) return Status::kInvalidArgument;
  uint8_t const* p = in.data();
  out->request = get_u64(p);
  out->region = get_u64(p + 8);
  out->generation = get_u32(p + 16);
  out->offset = get_u64(p + 20);
  out->length = get_u64(p + 28);
  return Status::kOk;
}

}  // namespace hux
