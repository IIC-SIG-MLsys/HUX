/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The mlx5 command encoding, held to what an adapter reported.
 *
 * A wrong offset does not fail: the adapter takes the command and applies
 * whatever those bits happen to mean. So the offsets are checked against a
 * queue pair context read back from a ConnectX-5 for a queue pair the
 * kernel brought up through verbs, with known attributes: MTU 1024, 16
 * outstanding reads each way, timeout 14, retries 7 and 7, RNR timer 12,
 * GID index 5, port 1. Only the words holding the fields used are kept, and
 * the destination MAC is zeroed. */
#include <cstring>
#include <vector>

#include "test_main.h"
#include "transport/rdma/mlx5_prm.h"

using namespace hux::mlx5prm;

namespace {

/* Queue pair 0x308, connected to 0x309, protection domain 0x16, completion
 * queue 0x540. Byte offset into the context, then the word found there. */
struct Word {
  unsigned byte;
  uint8_t b[4];
};
Word const kRead[] = {
    {0, {0x30, 0x00, 0x18, 0x00}},   {4, {0x00, 0x00, 0x00, 0x16}},
    {8, {0x7e, 0x20, 0x41, 0x20}},   {20, {0x00, 0x00, 0x03, 0x09}},
    {24, {0x00, 0x00, 0x00, 0x00}},  {32, {0x70, 0x05, 0x00, 0x40}},
    {56, {0x00, 0x80, 0xf3, 0x6c}},  {60, {0x00, 0x01, 0x00, 0x00}},
    {112, {0x80, 0x87, 0xe7, 0xe0}}, {120, {0x01, 0x00, 0x00, 0x00}},
    {124, {0x00, 0x00, 0x05, 0x40}}, {144, {0x00, 0x80, 0xc0, 0x00}},
    {148, {0x0c, 0x00, 0x00, 0x00}}, {156, {0x00, 0x00, 0x05, 0x40}},
};

std::vector<uint8_t> context() {
  std::vector<uint8_t> q(kQueryQpOutBytes - kQpcByteOffset, 0);
  for (auto const& w : kRead) std::memcpy(q.data() + w.byte, w.b, 4);
  return q;
}

struct Expect {
  Field f;
  uint32_t v;
  bool in_path; /* inside the primary address path */
};
Expect const kFields[] = {
    {kQpcState, kStateRts, false},
    {kQpcSt, kServiceRc, false},
    {kQpcPmState, kPathMigrated, false},
    {kQpcDpOrdering0, 0, false},
    {kQpcDpOrderingForce, 0, false},
    {kQpcPd, 0x16, false},
    {kQpcMtu, 3, false},
    {kQpcLogMsgMax, 30, false},
    {kQpcRemoteQpn, 0x309, false},
    {kQpcLogAckReqFreq, kAckReqFreq, false},
    {kQpcLogSraMax, 4, false},
    {kQpcRetryCount, 7, false},
    {kQpcRnrRetry, 7, false},
    {kQpcNextSendPsn, 0, false},
    {kQpcCqnSnd, 0x540, false},
    {kQpcLogRraMax, 4, false},
    {kQpcRre, 1, false},
    {kQpcRwe, 1, false},
    {kQpcRae, 0, false},
    {kQpcDpOrdering1, 0, false},
    {kQpcMinRnrNak, 12, false},
    {kQpcNextRcvPsn, 0, false},
    {kQpcCqnRcv, 0x540, false},
    {kAdsPkeyIndex, 0, true},
    {kAdsAckTimeout, 14, true},
    {kAdsSrcAddrIndex, 5, true},
    /* 64, not the 255 asked for: the kernel takes the route's. */
    {kAdsHopLimit, 64, true},
    {kAdsDscp, 0, true},
    {kAdsUdpSport, 0xf36c, true},
    {kAdsEthPrio, 0, true},
    {kAdsVhcaPortNum, 1, true},
};

}  // namespace

HUX_TEST(every_field_reads_what_the_adapter_reported) {
  std::vector<uint8_t> const q = context();
  for (auto const& e : kFields) {
    uint8_t const* base = q.data() + (e.in_path ? kAdsByteOffset : 0);
    CHECK_EQ(get(base, e.f), e.v);
  }
}

HUX_TEST(writing_a_field_leaves_its_neighbours_alone) {
  /* Written back with the value it already holds, a field must leave the
   * whole context as it was -- the words here are full of other fields'
   * bits. And written into zeroes, it must read back alone. */
  std::vector<uint8_t> q = context();
  std::vector<uint8_t> const before = q;
  for (auto const& e : kFields) {
    uint8_t* base = q.data() + (e.in_path ? kAdsByteOffset : 0);
    set(base, e.f, e.v);
  }
  CHECK(q == before);

  for (auto const& e : kFields) {
    std::vector<uint8_t> z(q.size(), 0);
    uint8_t* base = z.data() + (e.in_path ? kAdsByteOffset : 0);
    uint32_t const all = e.f.bits == 32 ? 0xffffffffu : (1u << e.f.bits) - 1;
    set(base, e.f, all);
    CHECK_EQ(get(base, e.f), all);
    /* Exactly the field's own bits went up. */
    size_t ones = 0;
    for (uint8_t b : z)
      for (int i = 0; i < 8; ++i) ones += (b >> i) & 1u;
    CHECK_EQ(ones, size_t(e.f.bits));
  }
}

HUX_TEST(a_value_wider_than_its_field_is_cut_to_it) {
  std::vector<uint8_t> q = context();
  set(q.data(), kQpcMtu, 0xff); /* 3 bits */
  CHECK_EQ(get(q.data(), kQpcMtu), 7u);
  /* log_msg_max shares the byte and was not touched. */
  CHECK_EQ(get(q.data(), kQpcLogMsgMax), 30u);
}

HUX_TEST(the_udp_source_port_is_the_kernels) {
  /* Both pairs as the kernel computed them for verbs queue pairs. */
  CHECK_EQ(roce_udp_sport(0x308, 0x309), uint16_t(0xf36c));
  CHECK_EQ(roce_udp_sport(0x302, 0x303), uint16_t(0xcf22));
  /* RoCE v2 reserves the range from 0xc000 for this. */
  CHECK(roce_udp_sport(0xffffff, 0xfffffe) >= 0xc000);
}
