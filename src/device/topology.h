/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Which NIC belongs with which device.
 *
 * On a machine with several of each, the pairing decides whether a transfer
 * crosses a socket, and that is worth more than most tuning. The answer comes
 * from sysfs rather than from configuration, because an operator who has to
 * supply it has to be right, and the information is already on the machine. */
#ifndef HUX_DEVICE_TOPOLOGY_H
#define HUX_DEVICE_TOPOLOGY_H

#include <cstdint>
#include <string>
#include <vector>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

/* A PCI address, as sysfs writes it: 0000:1a:00.0 */
struct PciAddress {
  uint32_t domain = 0;
  uint8_t bus = 0;
  uint8_t device = 0;
  uint8_t function = 0;

  std::string to_string() const;
  bool valid() const { return !(domain == 0 && bus == 0 && device == 0); }
};

Status parse_pci_address(std::string const& text, PciAddress* out);

/* How closely two endpoints sit, from nearest to furthest. Named after what
 * the data has to cross, since that is what costs. */
enum class Proximity : uint8_t {
  kSameDevice = 0,
  kSameBus,   /* same PCIe bus: usually one switch */
  kSameNuma,  /* same NUMA node, further up the hierarchy */
  kCrossNuma, /* different sockets: the interconnect is in the path */
  kUnknown,
};

char const* to_string(Proximity p);

/* Judged from PCI addresses and NUMA nodes alone. A shared bus is a strong
 * signal; beyond that only the NUMA node is reliable without walking the
 * whole PCIe tree, and a guess dressed up as a measurement is worse than
 * kUnknown. */
Proximity proximity(PciAddress const& a, int a_numa, PciAddress const& b,
                    int b_numa);

/* NUMA node of a PCI device, or -1 when the kernel does not say.
 *
 * Provided rather than left to the caller because the sysfs path is
 * case-sensitive while several vendor runtimes report the address in upper
 * case -- a mismatch that silently yields "unknown" for every device whose
 * address happens to contain a letter. */
int pci_numa_node(PciAddress const& pci);

struct NicInfo {
  std::string name; /* mlx5_0 */
  PciAddress pci;
  int numa_node = -1;
  bool active = false;    /* port state */
  std::string link_layer; /* ethernet or infiniband */
};

/* Every RDMA device sysfs knows about, whether or not it is usable. A NIC
 * that is present but down is worth reporting: it explains a machine that
 * looks equipped and behaves as though it is not. */
std::vector<NicInfo> discover_nics();

struct DeviceLocation {
  DeviceId device;
  PciAddress pci;
  int numa_node = -1;
};

/* Pairs a device with the NIC nearest to it. Returns false when nothing can
 * be established, rather than naming an arbitrary one. */
bool best_nic_for(DeviceLocation const& dev, std::vector<NicInfo> const& nics,
                  NicInfo* out, Proximity* how_close);

/* The whole picture as machine-readable text, for a run to record. */
std::string describe_topology(std::vector<DeviceLocation> const& devices);

}  // namespace hux
#endif  // HUX_DEVICE_TOPOLOGY_H
