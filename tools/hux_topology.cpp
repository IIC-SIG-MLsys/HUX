/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Prints the machine's RDMA topology and pairs each accelerator with the
 * nearest usable NIC. Run it before tuning anything: on a machine with
 * several of each, which NIC a device uses decides whether transfers cross a
 * socket, and that outweighs most other settings.
 *
 * Build for the vendor present, e.g.
 *   g++ -std=c++17 hux_topology.cpp ../src/device/topology.cpp -o hux-topology
 * \ -I../src -I../include -DHUX_TOPO_CUDA -I/usr/local/cuda/include -lcudart
 * Without a vendor macro it reports NICs only, which needs nothing installed.
 */
#include <cstdio>
#include <string>
#include <vector>

#include "device/topology.h"

#if defined(HUX_TOPO_CUDA)
#include <cuda_runtime.h>
#elif defined(HUX_TOPO_ROCM)
#include <hip/hip_runtime.h>
#elif defined(HUX_TOPO_NEUWARE)
#include <cnrt.h>
#endif

using namespace hux;

namespace {

struct Accel {
  std::string name;
  DeviceLocation loc;
};

std::vector<Accel> discover_accelerators() {
  std::vector<Accel> out;
#if defined(HUX_TOPO_CUDA)
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess) return out;
  for (int i = 0; i < count; ++i) {
    char bus[32] = {0};
    cudaDeviceProp prop{};
    if (cudaDeviceGetPCIBusId(bus, sizeof bus, i) != cudaSuccess) continue;
    cudaGetDeviceProperties(&prop, i);
    Accel a;
    a.name = prop.name;
    a.loc.device = DeviceId{DeviceKind::kCuda, i};
    parse_pci_address(bus, &a.loc.pci);
    a.loc.numa_node = pci_numa_node(a.loc.pci);
    out.push_back(std::move(a));
  }
#elif defined(HUX_TOPO_ROCM)
  int count = 0;
  if (hipGetDeviceCount(&count) != hipSuccess) return out;
  for (int i = 0; i < count; ++i) {
    char bus[32] = {0};
    hipDeviceProp_t prop{};
    if (hipDeviceGetPCIBusId(bus, sizeof bus, i) != hipSuccess) continue;
    hipGetDeviceProperties(&prop, i);
    Accel a;
    a.name = prop.name;
    a.loc.device = DeviceId{DeviceKind::kRocm, i};
    parse_pci_address(bus, &a.loc.pci);
    a.loc.numa_node = pci_numa_node(a.loc.pci);
    out.push_back(std::move(a));
  }
#elif defined(HUX_TOPO_NEUWARE)
  unsigned count = 0;
  if (cnrtGetDeviceCount(&count) != cnrtSuccess) return out;
  for (unsigned i = 0; i < count; ++i) {
    Accel a;
    a.name = "Cambricon MLU";
    a.loc.device = DeviceId{DeviceKind::kCambricon, static_cast<int>(i)};
    /* CNRT exposes no PCI address, so only what sysfs can be asked about is
     * reported -- an empty address rather than a fabricated one. */
    out.push_back(std::move(a));
  }
#endif
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  bool const as_json = argc > 1 && std::string(argv[1]) == "--json";

  std::vector<NicInfo> const nics = discover_nics();
  std::vector<Accel> const accels = discover_accelerators();

  if (as_json) {
    std::vector<DeviceLocation> locs;
    locs.reserve(accels.size());
    for (auto const& a : accels) locs.push_back(a.loc);
    std::printf("%s\n", describe_topology(locs).c_str());
    return 0;
  }

  std::printf("RDMA devices:\n");
  if (nics.empty()) std::printf("  (none)\n");
  for (auto const& n : nics) {
    std::printf("  %-10s %s  numa=%-3d %-12s %s\n", n.name.c_str(),
                n.pci.to_string().c_str(), n.numa_node, n.link_layer.c_str(),
                n.active ? "ACTIVE" : "down");
  }

  std::printf("\nAccelerators and their nearest usable NIC:\n");
  if (accels.empty()) {
    std::printf("  (none; built without a vendor runtime)\n");
    return 0;
  }
  for (auto const& a : accels) {
    NicInfo nic;
    Proximity how = Proximity::kUnknown;
    bool const found = best_nic_for(a.loc, nics, &nic, &how);
    std::printf("  %-24s %s numa=%-3d -> %-10s %s\n", a.name.c_str(),
                a.loc.pci.to_string().c_str(), a.loc.numa_node,
                found ? nic.name.c_str() : "(none)", to_string(how));
  }
  /* A pairing this reports as cross_numa is worth acting on; one it reports
   * as unknown means the machine did not say, not that the devices are far
   * apart. */
  return 0;
}
