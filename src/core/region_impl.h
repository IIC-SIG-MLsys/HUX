/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_CORE_REGION_IMPL_H
#define HUX_CORE_REGION_IMPL_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "control/identity.h"
#include "hux/region.h"

namespace hux {

/* Decoded descriptor. Field widths and byte order are fixed by the codec, so
 * nothing depends on the local ABI and no raw C++ struct goes on the wire. */
/* A remote key together with the provider that minted it. */
struct ProviderKey {
  std::string provider;
  uint64_t remote_key = 0;
};

struct RegionDescriptor {
  uint16_t major = 0;
  uint16_t minor = 0;
  /* The engine that exported it. Region ids are handed out per engine from
   * one, so an id says nothing about where it came from: a peer that
   * restarts numbers its regions from the beginning again and its old
   * descriptors go on looking current. Carrying the origin is what lets an
   * import refuse them, instead of the hardware refusing the transfer later
   * with a message about a key. */
  Identity origin;
  RegionId region = 0;
  Generation generation = 0;
  uint64_t base = 0; /* Peer virtual address; never dereferenced here. */
  uint64_t length = 0;
  /* The first provider's key, kept as its own field so the common case reads
   * the same as it always did. */
  uint64_t remote_key = 0;
  /* One key per provider that registered this region. A peer reached over IPC
   * cannot use an rkey minted by a NIC, so the descriptor carries every key
   * and the importer takes the one for the path it arrived on -- rather than
   * taking the only key on offer and failing later, somewhere that says
   * nothing about why. */
  std::vector<ProviderKey> provider_keys;
  DeviceKind device_kind = DeviceKind::kHost;
  int32_t device_index = 0;
  AccessFlags access = AccessFlags::kNone;
};

/* Little-endian, fixed-width fields. */
void encode_descriptor(RegionDescriptor const& d, std::vector<uint8_t>* out);
/* Rejects a major mismatch outright instead of parsing best-effort. */
Status decode_descriptor(std::vector<uint8_t> const& buf,
                         RegionDescriptor* out);

/* One underlying registration, shared by every handle whose range falls
 * inside it. Released when the last of them goes, which is what lets a pool
 * be registered once and transferred by views. */
struct Registration {
  void* base = nullptr;
  uint64_t length = 0;
  DeviceId device;
  AccessFlags access = AccessFlags::kNone;

  /* What one provider gave back for this range. A region is registered with
   * every provider the engine holds, because which one a peer will be reached
   * over is not known when the memory is registered. */
  struct Key {
    std::string provider;
    uint64_t local_key = 0;
    uint64_t remote_key = 0;
  };
  std::vector<Key> keys;

  /* The first provider's, which is the only one when there is only one. */
  uint64_t local_key = 0;
  uint64_t remote_key = 0;

  /* The local key for a named provider, or none if that provider refused this
   * memory -- IPC refuses host memory, so this genuinely happens. */
  bool local_key_for(std::string const& provider, uint64_t* out) const {
    for (auto const& k : keys) {
      if (k.provider == provider) {
        if (out != nullptr) *out = k.local_key;
        return true;
      }
    }
    return false;
  }
};
using RegistrationPtr = std::shared_ptr<Registration>;

/* Whether a request can be served by an existing registration.
 *
 * Containment has to be exact. A range that only partly overlaps covers bytes
 * the hardware was never told about, and the transfer that follows fails
 * somewhere far from the registration that caused it. Permissions have to be
 * at least as wide, for the same reason in the other direction -- and the
 * remote ones exactly as wide, since the key a handle exports lets a peer do
 * whatever its registration allows. */
bool registration_covers(Registration const& r, void* addr, uint64_t length,
                         DeviceId device, AccessFlags access);

class MemoryRegionImpl : public MemoryRegion {
 public:
  MemoryRegionImpl(Identity origin, RegionId id, Generation gen, void* base,
                   uint64_t length, DeviceId dev, MemoryKind mem,
                   AccessFlags access, RegistrationPtr reg);

  RegionId id() const override { return id_; }
  Identity origin() const { return origin_; }
  Generation generation() const override { return gen_; }
  void* base() const override { return base_; }
  uint64_t length() const override { return length_; }
  DeviceId device() const override { return dev_; }
  MemoryKind memory_kind() const override { return mem_; }
  AccessFlags access() const override { return access_; }
  Status view(uint64_t offset, uint64_t length, RegionView* out) const override;
  Status export_descriptor(std::vector<uint8_t>* out) const override;

  uint64_t local_key() const { return reg_->local_key; }
  /* The key from a named transport, when that transport registered this
   * memory at all. */
  bool local_key_for(std::string const& provider, uint64_t* out) const {
    return reg_->local_key_for(provider, out);
  }
  uint64_t remote_key() const { return reg_->remote_key; }
  RegistrationPtr const& registration() const { return reg_; }

  /* Deregistration blocks new submissions first; this is that step. */
  void retire() { retired_.store(true, std::memory_order_release); }
  bool retired() const { return retired_.load(std::memory_order_acquire); }

 private:
  Identity const origin_;
  RegionId const id_;
  Generation const gen_;
  void* const base_;
  uint64_t const length_;
  DeviceId const dev_;
  MemoryKind const mem_;
  AccessFlags const access_;
  RegistrationPtr const reg_;
  std::atomic<bool> retired_{false};
};

class RemoteRegionImpl : public RemoteRegion {
 public:
  explicit RemoteRegionImpl(RegionDescriptor const& d);

  RegionId id() const override { return d_.region; }
  Generation generation() const override { return d_.generation; }
  uint64_t length() const override { return d_.length; }
  DeviceId device() const override;
  AccessFlags access() const override { return d_.access; }
  bool valid() const override { return valid_.load(std::memory_order_acquire); }
  Status view(uint64_t offset, uint64_t length, RegionView* out) const override;

  uint64_t base() const { return d_.base; }
  uint64_t remote_key() const { return d_.remote_key; }
  /* The engine that exported it, which is how a notice that it has gone
   * finds it. */
  Identity const& origin() const { return d_.origin; }
  /* The key the named transport minted. A peer with an adapter each side of
   * a machine exported one per adapter, and a write carried by one of them
   * cannot use the other's: an rkey belongs to the protection domain that
   * issued it. Falls back to the unnamed key for a peer that exported only
   * one. */
  bool remote_key_for(std::string const& provider, uint64_t* out) const {
    for (auto const& k : d_.provider_keys)
      if (k.provider == provider) {
        *out = k.remote_key;
        return true;
      }
    if (d_.provider_keys.empty()) {
      *out = d_.remote_key;
      return true;
    }
    return false;
  }
  void invalidate() { valid_.store(false, std::memory_order_release); }

 private:
  RegionDescriptor const d_;
  std::atomic<bool> valid_{true};
};

}  // namespace hux
#endif  // HUX_CORE_REGION_IMPL_H
