/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "device/topology.h"

#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace hux {
namespace {

/* Reads a sysfs file, trimmed. Missing files are normal here -- the layout
 * differs between drivers and kernel versions -- so absence is reported as an
 * empty string rather than an error. */
std::string read_sysfs(std::string const& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::string line;
  std::getline(f, line);
  while (!line.empty() && (line.back() == '\n' || line.back() == ' '))
    line.pop_back();
  return line;
}

int read_numa(std::string const& dir) {
  std::string const v = read_sysfs(dir + "/numa_node");
  if (v.empty()) return -1;
  int n = std::atoi(v.c_str());
  /* -1 means the kernel has no NUMA information, which is different from node
   * zero and must not be treated as it. */
  return n;
}

}  // namespace

std::string PciAddress::to_string() const {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04x:%02x:%02x.%x", domain, bus, device,
                function);
  return buf;
}

Status parse_pci_address(std::string const& text, PciAddress* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  unsigned domain = 0, bus = 0, dev = 0, fn = 0;
  if (std::sscanf(text.c_str(), "%x:%x:%x.%x", &domain, &bus, &dev, &fn) != 4)
    return Status::kInvalidArgument;
  out->domain = domain;
  out->bus = static_cast<uint8_t>(bus);
  out->device = static_cast<uint8_t>(dev);
  out->function = static_cast<uint8_t>(fn);
  return Status::kOk;
}

char const* to_string(Proximity p) {
  switch (p) {
    case Proximity::kSameDevice:
      return "same_device";
    case Proximity::kSameBus:
      return "same_bus";
    case Proximity::kSameNuma:
      return "same_numa";
    case Proximity::kCrossNuma:
      return "cross_numa";
    case Proximity::kUnknown:
      return "unknown";
  }
  return "unknown";
}

Proximity proximity(PciAddress const& a, int a_numa, PciAddress const& b,
                    int b_numa) {
  if (a.valid() && b.valid()) {
    if (a.domain == b.domain && a.bus == b.bus && a.device == b.device)
      return Proximity::kSameDevice;
    if (a.domain == b.domain && a.bus == b.bus) return Proximity::kSameBus;
  }
  /* Node -1 means the kernel does not know, which is not the same as node 0.
   * Comparing them would invent a relationship. */
  if (a_numa < 0 || b_numa < 0) return Proximity::kUnknown;
  return a_numa == b_numa ? Proximity::kSameNuma : Proximity::kCrossNuma;
}

int pci_numa_node(PciAddress const& pci) {
  if (!pci.valid()) return -1;
  /* to_string() writes lower case, which is what sysfs uses. A runtime that
   * hands back upper case would otherwise find nothing here. */
  return read_numa("/sys/bus/pci/devices/" + pci.to_string());
}

std::vector<NicInfo> discover_nics() {
  std::vector<NicInfo> out;
  char const* kRoot = "/sys/class/infiniband";
  DIR* d = ::opendir(kRoot);
  if (d == nullptr) return out;

  while (dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.') continue;
    NicInfo n;
    n.name = e->d_name;
    std::string const base = std::string(kRoot) + "/" + n.name;
    std::string const dev = base + "/device";

    /* The PCI address is the directory the device symlink points at. */
    char resolved[512];
    ssize_t len = ::readlink(dev.c_str(), resolved, sizeof(resolved) - 1);
    if (len > 0) {
      resolved[len] = '\0';
      std::string const target(resolved);
      size_t const slash = target.find_last_of('/');
      std::string const leaf =
          slash == std::string::npos ? target : target.substr(slash + 1);
      parse_pci_address(leaf, &n.pci);
    }
    n.numa_node = read_numa(dev);

    /* Port 1 is what this project uses; a device with only other ports up
     * reports inactive, which is honest rather than convenient. */
    std::string const port = base + "/ports/1";
    std::string const state = read_sysfs(port + "/state");
    n.active = state.find("ACTIVE") != std::string::npos;
    std::string const ll = read_sysfs(port + "/link_layer");
    n.link_layer = ll.empty() ? "unknown" : ll;

    out.push_back(std::move(n));
  }
  ::closedir(d);
  return out;
}

bool best_nic_for(DeviceLocation const& dev, std::vector<NicInfo> const& nics,
                  NicInfo* out, Proximity* how_close) {
  if (out == nullptr) return false;
  NicInfo const* best = nullptr;
  Proximity best_p = Proximity::kUnknown;

  for (auto const& n : nics) {
    /* A NIC that is down cannot carry anything, however close it sits. */
    if (!n.active) continue;
    Proximity const p = proximity(dev.pci, dev.numa_node, n.pci, n.numa_node);
    if (p == Proximity::kUnknown) continue;
    if (best == nullptr || p < best_p) {
      best = &n;
      best_p = p;
    }
  }
  if (best == nullptr) return false; /* say nothing rather than guess */
  *out = *best;
  if (how_close != nullptr) *how_close = best_p;
  return true;
}

std::string describe_topology(std::vector<DeviceLocation> const& devices) {
  std::vector<NicInfo> const nics = discover_nics();
  std::ostringstream o;
  o << "{\"nics\":[";
  for (size_t i = 0; i < nics.size(); ++i) {
    if (i) o << ',';
    o << "{\"name\":\"" << nics[i].name << "\",\"pci\":\""
      << nics[i].pci.to_string() << "\",\"numa\":" << nics[i].numa_node
      << ",\"active\":" << (nics[i].active ? "true" : "false")
      << ",\"link_layer\":\"" << nics[i].link_layer << "\"}";
  }
  o << "],\"devices\":[";
  for (size_t i = 0; i < devices.size(); ++i) {
    if (i) o << ',';
    NicInfo nic;
    Proximity how = Proximity::kUnknown;
    bool const found = best_nic_for(devices[i], nics, &nic, &how);
    o << "{\"pci\":\"" << devices[i].pci.to_string()
      << "\",\"numa\":" << devices[i].numa_node << ",\"nearest_nic\":";
    if (found) {
      o << "\"" << nic.name << "\",\"proximity\":\"" << to_string(how) << "\"";
    } else {
      /* No pairing could be established. Naming one anyway would be a guess
       * that reads like a measurement. */
      o << "null,\"proximity\":\"unknown\"";
    }
    o << "}";
  }
  o << "]}";
  return o.str();
}

}  // namespace hux
