/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Pairing judgements, on fabricated addresses so they hold on any machine.
 * The cases worth pinning are the ones where saying nothing is the correct
 * answer: a guess that reads like a measurement is worse than no answer. */
#include <string>
#include <vector>

#include "device/topology.h"
#include "test_main.h"

using namespace hux;

namespace {

PciAddress addr(char const* text) {
  PciAddress a;
  parse_pci_address(text, &a);
  return a;
}

NicInfo nic(char const* name, char const* pci, int numa, bool active) {
  NicInfo n;
  n.name = name;
  n.pci = addr(pci);
  n.numa_node = numa;
  n.active = active;
  n.link_layer = "Ethernet";
  return n;
}

}  // namespace

HUX_TEST(pci_addresses_round_trip) {
  PciAddress a;
  CHECK_STATUS(parse_pci_address("0000:da:00.0", &a), Status::kOk);
  CHECK_EQ(a.domain, 0u);
  CHECK_EQ(a.bus, 0xdau);
  /* Written back in lower case, which is what sysfs paths use. A runtime
   * reporting upper case would otherwise find nothing. */
  CHECK(a.to_string() == "0000:da:00.0");
}

HUX_TEST(pci_parse_rejects_nonsense) {
  PciAddress a;
  CHECK_STATUS(parse_pci_address("not-an-address", &a),
               Status::kInvalidArgument);
  CHECK_STATUS(parse_pci_address("", &a), Status::kInvalidArgument);
}

HUX_TEST(two_functions_of_one_card_are_the_same_device) {
  /* A dual-port NIC appears as two functions of one device, not as two
   * devices sharing a bus. */
  CHECK(proximity(addr("0000:5e:00.0"), 0, addr("0000:5e:00.1"), 0) ==
        Proximity::kSameDevice);
}

HUX_TEST(a_shared_bus_ranks_above_a_shared_numa_node) {
  CHECK(proximity(addr("0000:5e:00.0"), 0, addr("0000:5e:01.0"), 0) ==
        Proximity::kSameBus);
  CHECK(proximity(addr("0000:1a:00.0"), 0, addr("0000:01:00.0"), 0) ==
        Proximity::kSameNuma);
  CHECK(proximity(addr("0000:1a:00.0"), 0, addr("0000:da:00.0"), 1) ==
        Proximity::kCrossNuma);
}

HUX_TEST(an_unknown_numa_node_is_not_node_zero) {
  /* The kernel writes -1 when it has no information. Comparing that against 0
   * would invent a relationship and report it as measured. */
  CHECK(proximity(addr("0000:1a:00.0"), -1, addr("0000:01:00.0"), 0) ==
        Proximity::kUnknown);
  CHECK(proximity(addr("0000:1a:00.0"), 0, addr("0000:01:00.0"), -1) ==
        Proximity::kUnknown);
}

HUX_TEST(a_down_nic_is_never_chosen) {
  /* However close it sits, it cannot carry anything. */
  std::vector<NicInfo> nics = {nic("near_down", "0000:1a:00.1", 0, false),
                               nic("far_up", "0000:da:00.0", 1, true)};
  DeviceLocation dev;
  dev.pci = addr("0000:1a:00.0");
  dev.numa_node = 0;

  NicInfo chosen;
  Proximity how = Proximity::kUnknown;
  CHECK(best_nic_for(dev, nics, &chosen, &how));
  CHECK(chosen.name == "far_up");
  CHECK(how == Proximity::kCrossNuma);
}

HUX_TEST(no_pairing_is_reported_rather_than_guessed) {
  /* With no NUMA information on either side there is nothing to judge by.
   * Naming a NIC anyway would read like a measurement. */
  std::vector<NicInfo> nics = {nic("mlx5_0", "0000:01:00.0", -1, true)};
  DeviceLocation dev;
  dev.pci = addr("0000:1a:00.0");
  dev.numa_node = -1;

  NicInfo chosen;
  CHECK(!best_nic_for(dev, nics, &chosen, nullptr));
}

HUX_TEST(the_nearest_of_several_is_taken) {
  std::vector<NicInfo> nics = {nic("far", "0000:da:00.0", 1, true),
                               nic("same_numa", "0000:01:00.0", 0, true),
                               nic("same_bus", "0000:1a:01.0", 0, true)};
  DeviceLocation dev;
  dev.pci = addr("0000:1a:00.0");
  dev.numa_node = 0;

  NicInfo chosen;
  Proximity how = Proximity::kUnknown;
  CHECK(best_nic_for(dev, nics, &chosen, &how));
  CHECK(chosen.name == "same_bus");
  CHECK(how == Proximity::kSameBus);
}
