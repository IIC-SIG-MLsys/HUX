/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Just enough of the mlx5 command layout to bring a verbs queue pair up with
 * one field verbs does not expose: out-of-order placement of RDMA reads and
 * writes.
 *
 * Offsets are in bits, numbered as the device's programming reference
 * numbers them, and every field used here lies inside one big-endian 32-bit
 * word. They were taken from UCX's copy of the layout and checked, field by
 * field, against the kernel's. Two are named only by UCX -- the queue pair's
 * request for out-of-order placement and the capability bit that allows it --
 * and they sit in bits the kernel's copy leaves reserved.
 *
 * No dependency on the mlx5 headers, so the encoding can be tested on a
 * machine without them. */
#ifndef HUX_TRANSPORT_RDMA_MLX5_PRM_H
#define HUX_TRANSPORT_RDMA_MLX5_PRM_H

#include <cstdint>

namespace hux {
namespace mlx5prm {

struct Field {
  unsigned off;
  unsigned bits;
};

inline void set(void* buf, Field f, uint32_t v) {
  auto* p = static_cast<uint8_t*>(buf) + (f.off / 32) * 4;
  unsigned const shift = 32 - (f.off % 32) - f.bits;
  uint32_t const mask = (f.bits == 32 ? 0xffffffffu : ((1u << f.bits) - 1))
                        << shift;
  uint32_t w = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  w = (w & ~mask) | ((v << shift) & mask);
  p[0] = uint8_t(w >> 24);
  p[1] = uint8_t(w >> 16);
  p[2] = uint8_t(w >> 8);
  p[3] = uint8_t(w);
}

inline uint32_t get(void const* buf, Field f) {
  auto const* p = static_cast<uint8_t const*>(buf) + (f.off / 32) * 4;
  unsigned const shift = 32 - (f.off % 32) - f.bits;
  uint32_t const mask = f.bits == 32 ? 0xffffffffu : ((1u << f.bits) - 1);
  uint32_t const w = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                     (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  return (w >> shift) & mask;
}

/* The UDP source port the kernel gives a RoCE v2 queue pair with no flow
 * label, so a queue pair brought up here hashes onto the same paths as one
 * brought up through verbs. */
inline uint16_t roce_udp_sport(uint32_t local_qpn, uint32_t remote_qpn) {
  uint64_t v = uint64_t(local_qpn) * remote_qpn;
  v ^= v >> 20;
  v ^= v >> 40;
  uint32_t const label = uint32_t(v & 0xfffff);
  uint32_t low = label & 0x3fff;
  low ^= (label & 0xfc000) >> 14;
  return uint16_t(low | 0xc000);
}

/* Command opcodes. */
constexpr uint16_t kQueryHcaCap = 0x100;
constexpr uint16_t kInit2RtrQp = 0x503;
constexpr uint16_t kRtr2RtsQp = 0x504;
constexpr uint16_t kQueryQp = 0x50b;

/* QUERY_HCA_CAP: which capability page, current values. */
constexpr uint16_t kCapGeneral = (0x00 << 1) | 1;

/* Sizes, in bytes, of the commands used. */
constexpr unsigned kQueryHcaCapInBytes = 16;
constexpr unsigned kQueryHcaCapOutBytes = 4112;
constexpr unsigned kModifyQpInBytes = 272;
constexpr unsigned kModifyQpOutBytes = 16;
constexpr unsigned kQueryQpInBytes = 16;
constexpr unsigned kQueryQpOutBytes = 272;

/* Command header, common to all of them. */
constexpr Field kOpcode{0x00, 16};
constexpr Field kOpMod{0x30, 16};
constexpr Field kStatus{0x00, 8};
constexpr Field kSyndrome{0x20, 32};
constexpr Field kQpn{0x48, 24};
constexpr Field kOptParamMask{0x80, 32};
/* Where the capability page starts in QUERY_HCA_CAP's output, and the queue
 * pair context in a modify's input or QUERY_QP's output. */
constexpr unsigned kCapByteOffset = 0x80 / 8;
constexpr unsigned kQpcByteOffset = 0xc0 / 8;

/* The general capability page. */
constexpr Field kCapLogMaxMsg{0x1c3, 5};
constexpr Field kCapOooRwRc{0x1ca, 1};

/* Optional parameters of INIT2RTR that it sets. */
constexpr uint32_t kOptRre = 1u << 1;
constexpr uint32_t kOptRae = 1u << 2;
constexpr uint32_t kOptRwe = 1u << 3;

/* Queue pair context. */
constexpr Field kQpcState{0x00, 4};
constexpr Field kQpcSt{0x08, 8};
constexpr Field kQpcPmState{0x13, 2};
constexpr Field kQpcDpOrdering0{0x26, 1};
constexpr Field kQpcDpOrderingForce{0x27, 1};
constexpr Field kQpcPd{0x28, 24};
constexpr Field kQpcMtu{0x40, 3};
constexpr Field kQpcLogMsgMax{0x43, 5};
constexpr Field kQpcRemoteQpn{0xa8, 24};
constexpr Field kQpcLogAckReqFreq{0x380, 4};
constexpr Field kQpcLogSraMax{0x388, 3};
constexpr Field kQpcRetryCount{0x38d, 3};
constexpr Field kQpcRnrRetry{0x390, 3};
constexpr Field kQpcNextSendPsn{0x3c8, 24};
constexpr Field kQpcCqnSnd{0x3e8, 24};
constexpr Field kQpcLogRraMax{0x488, 3};
constexpr Field kQpcRre{0x490, 1};
constexpr Field kQpcRwe{0x491, 1};
constexpr Field kQpcRae{0x492, 1};
constexpr Field kQpcDpOrdering1{0x49c, 1};
constexpr Field kQpcMinRnrNak{0x4a3, 5};
constexpr Field kQpcNextRcvPsn{0x4a8, 24};
constexpr Field kQpcCqnRcv{0x4e8, 24};
/* The primary address path, which starts at bit 0xc0 of the context. */
constexpr unsigned kAdsByteOffset = 0xc0 / 8;

/* Address path. */
constexpr Field kAdsPkeyIndex{0x10, 16};
constexpr Field kAdsAckTimeout{0x40, 5};
constexpr Field kAdsSrcAddrIndex{0x48, 8};
constexpr Field kAdsHopLimit{0x58, 8};
constexpr unsigned kAdsRgidByteOffset = 0x80 / 8; /* 16 bytes */
constexpr Field kAdsDscp{0x10a, 6};
constexpr Field kAdsUdpSport{0x110, 16};
constexpr Field kAdsEthPrio{0x121, 3};
constexpr Field kAdsVhcaPortNum{0x128, 8};
constexpr unsigned kAdsRmacByteOffset = 0x130 / 8; /* 6 bytes */

/* Values. */
constexpr uint32_t kStateRts = 3;
constexpr uint32_t kServiceRc = 0x0;
constexpr uint32_t kPathMigrated = 0x3;
/* What the kernel sets on every queue pair it modifies. */
constexpr uint32_t kAckReqFreq = 8;

}  // namespace mlx5prm
}  // namespace hux
#endif  // HUX_TRANSPORT_RDMA_MLX5_PRM_H
