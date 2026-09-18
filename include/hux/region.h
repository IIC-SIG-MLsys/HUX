// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_REGION_H
#define HUX_REGION_H

#include <cstdint>
#include <memory>
#include <vector>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

// 区域描述符的线格式版本。新旧 major 不兼容时明确拒绝握手，
// 不直接发送依赖本机 ABI 的裸 C++ struct。
constexpr uint16_t kDescriptorMajor = 1;
constexpr uint16_t kDescriptorMinor = 0;

// 一次传输涉及的一段范围。本地与远端分段按项配对、长度必须相等。
struct RegionView {
  RegionId region = 0;
  Span span;
};

// 注册应用已有内存后得到的句柄。
//
// 所有权：HUX 不拥有底层内存。C++ 侧要求调用方在注销前保持 allocation 有效；
// Python 侧由绑定层保留 Tensor 引用，调用方不得提前 resize 或重分配其存储。
class MemoryRegion {
 public:
  virtual ~MemoryRegion() = default;

  virtual RegionId id() const = 0;
  virtual Generation generation() const = 0;
  virtual void* base() const = 0;
  virtual uint64_t length() const = 0;
  virtual DeviceId device() const = 0;
  virtual MemoryKind memory_kind() const = 0;
  virtual AccessFlags access() const = 0;

  // 取一段子范围用于传输。越界或整数溢出返回 kOutOfRange。
  virtual Status view(uint64_t offset, uint64_t length,
                      RegionView* out) const = 0;

  // 导出可发送给对端的描述符。采用版本化编码，含长度、权限、设备、generation。
  virtual Status export_descriptor(std::vector<uint8_t>* out) const = 0;
};

using MemoryRegionPtr = std::shared_ptr<MemoryRegion>;

// 对端导出的区域。失效后禁止新提交。
//
// generation 只能防止软件使用旧描述符，**挡不住已经持有旧 rkey 的硬件访问**。
// 复用物理内存之前必须完成相关通道 drain 与注册撤销这类实际隔离。
class RemoteRegion {
 public:
  virtual ~RemoteRegion() = default;

  virtual RegionId id() const = 0;
  virtual Generation generation() const = 0;
  virtual uint64_t length() const = 0;
  virtual DeviceId device() const = 0;
  virtual AccessFlags access() const = 0;
  virtual bool valid() const = 0;

  virtual Status view(uint64_t offset, uint64_t length,
                      RegionView* out) const = 0;
};

using RemoteRegionPtr = std::shared_ptr<RemoteRegion>;

// 批量注册/注销的逐项结果。部分失败不丢失已成功项的所有权。
struct RegistrationResult {
  Status status = Status::kOk;
  MemoryRegionPtr region;  // 失败时为空
};

}  // namespace hux
#endif  // HUX_REGION_H
