// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_CORE_REGION_IMPL_H
#define HUX_CORE_REGION_IMPL_H

#include <atomic>
#include <cstdint>
#include <vector>

#include "hux/region.h"

namespace hux {

// 区域描述符的解码结果。字段宽度、字节序都在 codec 里写死，
// 不依赖本机 ABI，也不直接 memcpy 一个 C++ struct 出去。
struct RegionDescriptor {
  uint16_t major = 0;
  uint16_t minor = 0;
  RegionId region = 0;
  Generation generation = 0;
  uint64_t base = 0;      // 对端虚拟地址，本地不得解引用
  uint64_t length = 0;
  uint64_t remote_key = 0;
  DeviceKind device_kind = DeviceKind::kHost;
  int32_t device_index = 0;
  AccessFlags access = AccessFlags::kNone;
};

// 小端编码，定长字段 + 显式长度前缀。
void encode_descriptor(RegionDescriptor const& d, std::vector<uint8_t>* out);
// 校验版本与长度；major 不匹配直接拒绝，不尝试"尽力解析"。
Status decode_descriptor(std::vector<uint8_t> const& buf, RegionDescriptor* out);

class MemoryRegionImpl : public MemoryRegion {
 public:
  MemoryRegionImpl(RegionId id, Generation gen, void* base, uint64_t length,
                   DeviceId dev, MemoryKind mem, AccessFlags access,
                   uint64_t local_key, uint64_t remote_key);

  RegionId id() const override { return id_; }
  Generation generation() const override { return gen_; }
  void* base() const override { return base_; }
  uint64_t length() const override { return length_; }
  DeviceId device() const override { return dev_; }
  MemoryKind memory_kind() const override { return mem_; }
  AccessFlags access() const override { return access_; }
  Status view(uint64_t offset, uint64_t length, RegionView* out) const override;
  Status export_descriptor(std::vector<uint8_t>* out) const override;

  uint64_t local_key() const { return local_key_; }
  uint64_t remote_key() const { return remote_key_; }

  // 注销先阻止新提交，再 drain。这里只负责第一步。
  void retire() { retired_.store(true, std::memory_order_release); }
  bool retired() const { return retired_.load(std::memory_order_acquire); }

 private:
  RegionId const id_;
  Generation const gen_;
  void* const base_;
  uint64_t const length_;
  DeviceId const dev_;
  MemoryKind const mem_;
  AccessFlags const access_;
  uint64_t const local_key_;
  uint64_t const remote_key_;
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
  void invalidate() { valid_.store(false, std::memory_order_release); }

 private:
  RegionDescriptor const d_;
  std::atomic<bool> valid_{true};
};

}  // namespace hux
#endif  // HUX_CORE_REGION_IMPL_H
