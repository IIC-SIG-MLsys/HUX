/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "transport/rdma/mlx5_ordering.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "transport/rdma/mlx5_prm.h"

#ifdef HUX_HAVE_MLX5DV
#include <infiniband/mlx5dv.h>
#endif

namespace hux {

#ifdef HUX_HAVE_MLX5DV

namespace {

using namespace mlx5prm;

/* fls(n - 1), which is how the kernel turns a count of outstanding reads
 * into the context's log2 field. */
uint32_t log2_up(uint32_t n) {
  uint32_t l = 0;
  while (n > 1 && (1u << l) < n) ++l;
  return l;
}

bool query_qp(ibv_qp* qp, std::vector<uint8_t>* out) {
  uint8_t in[kQueryQpInBytes] = {};
  out->assign(kQueryQpOutBytes, 0);
  set(in, kOpcode, kQueryQp);
  set(in, kQpn, qp->qp_num);
  return mlx5dv_devx_qp_query(qp, in, sizeof(in), out->data(), out->size()) ==
         0;
}

/* The destination MAC, resolved by the kernel exactly as ibv_modify_qp would
 * resolve it, through an address handle made for the purpose. */
bool resolve_dmac(ibv_qp* qp, Mlx5Connect const& c, uint8_t mac[6]) {
  ibv_ah_attr aa{};
  aa.is_global = 1;
  std::memcpy(aa.grh.dgid.raw, c.dgid, 16);
  aa.grh.sgid_index = c.sgid_index;
  aa.grh.hop_limit = c.hop_limit;
  aa.port_num = c.port;
  ibv_ah* ah = ibv_create_ah(qp->pd, &aa);
  if (ah == nullptr) return false;
  mlx5dv_ah dah{};
  mlx5dv_obj obj{};
  obj.ah.in = ah;
  obj.ah.out = &dah;
  bool const ok =
      mlx5dv_init_obj(&obj, MLX5DV_OBJ_AH) == 0 && dah.av != nullptr;
  if (ok) std::memcpy(mac, dah.av->rmac, 6);
  ibv_destroy_ah(ah);
  return ok;
}

/* The fields the kernel writes into every modify. The protection domain and
 * completion queues are copied from the context the kernel built at INIT
 * rather than looked up: mlx5dv_init_obj on a completion queue marks it as
 * owned by the caller, and verbs then stops removing a destroyed queue
 * pair's completions from it -- the next poll finds one for a queue pair
 * that no longer exists and fails. */
void common_fields(uint8_t* qpc, uint8_t const* current) {
  set(qpc, kQpcSt, kServiceRc);
  set(qpc, kQpcPmState, kPathMigrated);
  set(qpc, kQpcPd, get(current, kQpcPd));
  set(qpc, kQpcCqnSnd, get(current, kQpcCqnSnd));
  set(qpc, kQpcCqnRcv, get(current, kQpcCqnRcv));
  set(qpc, kQpcLogAckReqFreq, kAckReqFreq);
}

/* A sysfs or procfs file's contents without surrounding whitespace, or
 * empty when it cannot be read. */
std::string read_file(std::string const& path) {
  std::ifstream f(path);
  if (!f) return std::string();
  std::stringstream ss;
  ss << f.rdbuf();
  std::string v = ss.str();
  size_t const b = v.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  size_t const e = v.find_last_not_of(" \t\r\n");
  return v.substr(b, e - b + 1);
}

}  // namespace

bool mlx5_kernel_path(char const* device, uint8_t port, uint8_t* hop_limit,
                      char const** why) {
  std::string const base = std::string("/sys/class/infiniband/") + device;
  std::string const p = std::to_string(port);
  if (!read_file(base + "/tc/" + p + "/traffic_class").empty()) {
    *why = "traffic class override";
    return false;
  }
  int const ttl = std::atoi(read_file(base + "/ttl/" + p + "/ttl").c_str());
  if (ttl > 0 && ttl <= 255) {
    *hop_limit = static_cast<uint8_t>(ttl);
  } else {
    /* The route's hop limit is the IPv4 default unless a route sets its own,
     * and the kernel takes 64 for anything below 2. */
    int const def =
        std::atoi(read_file("/proc/sys/net/ipv4/ip_default_ttl").c_str());
    *hop_limit = static_cast<uint8_t>(def >= 2 && def <= 255 ? def : 64);
  }
  *why = "";
  return true;
}

ibv_context* mlx5_open_device(ibv_device* device, bool* commands,
                              char const** why) {
  *commands = false;
  if (!mlx5dv_is_supported(device)) {
    *why = "not an mlx5 adapter";
    return ibv_open_device(device);
  }
  mlx5dv_context_attr attr{};
  attr.flags = MLX5DV_CONTEXT_FLAGS_DEVX;
  ibv_context* ctx = mlx5dv_open_device(device, &attr);
  if (ctx == nullptr) {
    *why = "device commands refused";
    return ibv_open_device(device);
  }
  *commands = true;
  *why = "";
  return ctx;
}

bool mlx5_ooo_rw_supported(ibv_context* ctx, uint32_t* log_max_msg,
                           char const** why) {
  mlx5dv_context dv{};
  dv.comp_mask = MLX5DV_CONTEXT_MASK_NUM_LAG_PORTS;
  if (mlx5dv_query_device(ctx, &dv) != 0) {
    *why = "adapter query failed";
    return false;
  }
  /* A bonded adapter picks a port per queue pair when it is brought up, and
   * nothing here has been run on one. */
  if ((dv.comp_mask & MLX5DV_CONTEXT_MASK_NUM_LAG_PORTS) != 0 &&
      dv.num_lag_ports > 1) {
    *why = "bonded adapter";
    return false;
  }
  uint8_t in[kQueryHcaCapInBytes] = {};
  std::vector<uint8_t> out(kQueryHcaCapOutBytes, 0);
  set(in, kOpcode, kQueryHcaCap);
  set(in, kOpMod, kCapGeneral);
  if (mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out.data(), out.size()) !=
      0) {
    *why = "capability query refused";
    return false;
  }
  uint8_t const* cap = out.data() + kCapByteOffset;
  if (get(cap, kCapOooRwRc) == 0) {
    *why = "adapter cannot";
    return false;
  }
  *log_max_msg = get(cap, kCapLogMaxMsg);
  *why = "";
  return true;
}

Status mlx5_connect_ooo_rw(ibv_qp* qp, Mlx5Connect const& c) {
  std::vector<uint8_t> now;
  if (!query_qp(qp, &now)) return Status::kDeviceError;
  uint8_t const* current = now.data() + kQpcByteOffset;

  uint8_t mac[6];
  if (!resolve_dmac(qp, c, mac)) return Status::kDeviceError;

  uint8_t rtr[kModifyQpInBytes] = {};
  uint8_t rtr_out[kModifyQpOutBytes] = {};
  set(rtr, kOpcode, kInit2RtrQp);
  set(rtr, kQpn, qp->qp_num);
  set(rtr, kOptParamMask, kOptRre | kOptRae | kOptRwe);
  uint8_t* qpc = rtr + kQpcByteOffset;
  common_fields(qpc, current);
  set(qpc, kQpcMtu, c.path_mtu);
  set(qpc, kQpcLogMsgMax, c.log_max_msg);
  set(qpc, kQpcRemoteQpn, c.remote_qpn);
  set(qpc, kQpcDpOrdering0, 1);
  set(qpc, kQpcLogRraMax, log2_up(c.max_dest_rd_atomic));
  /* Remote read and write, as the queue pair was given at INIT; no
   * atomics. */
  set(qpc, kQpcRre, 1);
  set(qpc, kQpcRwe, 1);
  set(qpc, kQpcRae, 0);
  set(qpc, kQpcMinRnrNak, c.min_rnr_timer);
  set(qpc, kQpcNextRcvPsn, 0);
  uint8_t* ads = qpc + kAdsByteOffset;
  set(ads, kAdsPkeyIndex, 0);
  std::memcpy(ads + kAdsRgidByteOffset, c.dgid, 16);
  set(ads, kAdsSrcAddrIndex, c.sgid_index);
  set(ads, kAdsHopLimit, c.hop_limit);
  std::memcpy(ads + kAdsRmacByteOffset, mac, 6);
  set(ads, kAdsUdpSport, roce_udp_sport(qp->qp_num, c.remote_qpn));
  set(ads, kAdsDscp, 0);
  set(ads, kAdsEthPrio, 0);
  set(ads, kAdsVhcaPortNum, c.port);
  if (mlx5dv_devx_qp_modify(qp, rtr, sizeof(rtr), rtr_out, sizeof(rtr_out)) !=
      0)
    return Status::kDeviceError;

  uint8_t rts[kModifyQpInBytes] = {};
  uint8_t rts_out[kModifyQpOutBytes] = {};
  set(rts, kOpcode, kRtr2RtsQp);
  set(rts, kQpn, qp->qp_num);
  qpc = rts + kQpcByteOffset;
  common_fields(qpc, current);
  set(qpc, kQpcLogSraMax, log2_up(c.max_rd_atomic));
  set(qpc, kQpcRetryCount, c.retry_cnt);
  set(qpc, kQpcRnrRetry, c.rnr_retry);
  set(qpc, kQpcNextSendPsn, 0);
  set(qpc + kAdsByteOffset, kAdsAckTimeout, c.timeout);
  if (mlx5dv_devx_qp_modify(qp, rts, sizeof(rts), rts_out, sizeof(rts_out)) !=
      0)
    return Status::kDeviceError;

  /* The kernel still believes the queue pair is in INIT, having not made
   * these transitions itself; verbs' own copy of the state is kept honest at
   * least, since nothing else will update it. */
  qp->state = IBV_QPS_RTS;

  /* Read back, because a field the adapter ignored would look exactly like
   * one it applied. */
  if (!query_qp(qp, &now)) return Status::kDeviceError;
  current = now.data() + kQpcByteOffset;
  if (get(current, kQpcState) != kStateRts ||
      get(current, kQpcDpOrdering0) != 1 || get(current, kQpcDpOrdering1) != 0)
    return Status::kDeviceError;
  return Status::kOk;
}

#else  // HUX_HAVE_MLX5DV

ibv_context* mlx5_open_device(ibv_device* device, bool* commands,
                              char const** why) {
  *commands = false;
  *why = "built without mlx5dv";
  return ibv_open_device(device);
}

bool mlx5_ooo_rw_supported(ibv_context*, uint32_t*, char const** why) {
  *why = "built without mlx5dv";
  return false;
}

bool mlx5_kernel_path(char const*, uint8_t, uint8_t*, char const** why) {
  *why = "built without mlx5dv";
  return false;
}

Status mlx5_connect_ooo_rw(ibv_qp*, Mlx5Connect const&) {
  return Status::kUnsupported;
}

#endif  // HUX_HAVE_MLX5DV

}  // namespace hux
