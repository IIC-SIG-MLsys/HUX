/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Out-of-order placement of RDMA reads and writes on mlx5 adapters.
 *
 * With the standard ordering a responder that misses one packet of a message
 * discards everything after it, until the sender goes back and resends from
 * the gap, and a rate cut follows. With out-of-order placement every packet
 * carries its own address and lands wherever it belongs whenever it arrives,
 * so a loss costs that packet. A sender faster than its receiver -- adapters
 * in slots of different widths, or of different generations -- is where the
 * difference shows: 1 MiB writes from a x8 adapter into a x4 one lost a
 * packet about one write in three and had a 1.5 ms tail, against none.
 *
 * Verbs cannot ask for it. rdma-core's MLX5DV_QP_CREATE_OOO_DP needs an
 * adapter that also reorders sends and receives, which ConnectX-5 does not
 * do. So the queue pair is created and taken to INIT through verbs, and to
 * RTR and RTS by the device's own commands, which carry the field. What
 * changes is the order in which a message's bytes land in memory;
 * completions still arrive in the order the work was posted. */
#ifndef HUX_TRANSPORT_RDMA_MLX5_ORDERING_H
#define HUX_TRANSPORT_RDMA_MLX5_ORDERING_H

#include <infiniband/verbs.h>

#include <cstdint>

#include "hux/status.h"

namespace hux {

/* Opens a device. An mlx5 adapter is opened with its command interface, so
 * a queue pair can later be brought up with fields verbs does not carry;
 * anything else, or an mlx5 adapter that refuses, gets a plain verbs
 * context. *commands says which it got, and *why, when it did not, the
 * reason. */
ibv_context* mlx5_open_device(ibv_device* device, bool* commands,
                              char const** why);

/* Whether RC queue pairs on this context can place reads and writes out of
 * order, and with it the longest message the adapter takes, which the
 * kernel writes into every queue pair it brings up. Needs a context opened
 * with the command interface. *why says what was missing when it cannot. */
bool mlx5_ooo_rw_supported(ibv_context* ctx, uint32_t* log_max_msg,
                           char const** why);

/* What the kernel writes into a RoCE address path that it takes from the
 * system rather than from the caller, since a queue pair brought up here
 * does not pass through it. The hop limit is the route's -- whatever the
 * caller asks for, the kernel replaces it when it resolves the destination
 * -- unless the adapter's TTL override is set. The traffic class can be
 * overridden too, globally or per destination, and that is not reproduced:
 * where it is set, false, so the queue pair stays with verbs rather than
 * landing in a different class from every other one. */
bool mlx5_kernel_path(char const* device, uint8_t port, uint8_t* hop_limit,
                      char const** why);

/* What INIT -> RTR -> RTS takes, as ibv_modify_qp would be given it. */
struct Mlx5Connect {
  uint32_t remote_qpn = 0;
  uint32_t path_mtu = IBV_MTU_1024;
  uint8_t port = 1;
  uint8_t sgid_index = 0;
  uint8_t hop_limit = 255;
  uint8_t dgid[16] = {};
  uint32_t max_dest_rd_atomic = 16;
  uint32_t max_rd_atomic = 16;
  uint8_t min_rnr_timer = 12;
  uint8_t timeout = 14;
  uint8_t retry_cnt = 7;
  uint8_t rnr_retry = 7;
  uint32_t log_max_msg = 30;
};

/* Takes an RC queue pair in INIT to RTS over RoCE v2 with out-of-order
 * placement of reads and writes, then reads its context back and fails
 * unless that is what the adapter holds. Everything else is set as the
 * kernel sets it for ibv_modify_qp, so the one difference between this queue
 * pair and a verbs one is the ordering. */
Status mlx5_connect_ooo_rw(ibv_qp* qp, Mlx5Connect const& c);

}  // namespace hux
#endif  // HUX_TRANSPORT_RDMA_MLX5_ORDERING_H
