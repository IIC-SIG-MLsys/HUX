// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_ENGINE_H
#define HUX_ENGINE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "hux/config.h"
#include "hux/device.h"
#include "hux/notification.h"
#include "hux/peer.h"
#include "hux/region.h"
#include "hux/request.h"
#include "hux/status.h"
#include "hux/types.h"

namespace hux {

// 一次传输的可选项。
struct TransferOptions {
  // 前置设备依赖：这些事件完成后才允许 NIC 读源区 / 写目标区。
  std::vector<DeviceEventPtr> after;

  // 非空时，该请求达到 target_ready 后向对端发布关联通知。
  // 数据失败或已接受的取消不会发出成功通知。
  std::vector<uint8_t> notify_payload;
  bool notify = false;

  void* context = nullptr;  // 仅本地关联，不发送给对端
};

// 通信引擎。一个进程可以有多个 Engine，彼此资源隔离；
// 每个 GPU 进程也可以只创建自己的那一个。
//
// 与旧 HMC 设计的根本区别：Engine 不拥有通信缓冲区。应用拥有内存，
// Engine 只负责注册应用已有的区域并在其上直接传输。
class Engine {
 public:
  virtual ~Engine() = default;

  static Status create(EngineConfig const& cfg,
                       std::shared_ptr<DeviceBackend> device,
                       std::unique_ptr<Engine>* out);

  virtual EngineConfig const& config() const = 0;

  // --- 注册 ---
  // 注册应用已有的内存，不要求应用改用本库的分配器。
  virtual Status register_memory(void* addr, uint64_t length,
                                 AccessFlags access,
                                 MemoryRegionPtr* out) = 0;
  // 批量注册逐项返回结果；部分失败不影响已成功项的所有权。
  virtual Status register_memory_batch(
      std::vector<void*> const& addrs, std::vector<uint64_t> const& lengths,
      AccessFlags access, std::vector<RegistrationResult>* out) = 0;
  // 注销先阻止新提交，再 drain 本地使用并协调远端访问。
  virtual Status deregister_memory(MemoryRegionPtr region) = 0;

  // --- 对端 ---
  virtual Status local_metadata(std::vector<uint8_t>* out) const = 0;
  // 重复添加同一个有效身份会复用既有连接，不会重复建连。
  virtual Status add_peer(std::vector<uint8_t> const& metadata,
                          PeerPtr* out) = 0;
  virtual Status remove_peer(PeerPtr peer) = 0;

  // --- 传输 ---
  // 全部默认异步返回 Request。标量版是单分段的便捷入口，内部复用同一条提交路径。
  // 一次向量操作面向一个 Peer，本地与远端分段按项配对、长度必须相等。
  virtual Status read(Peer* peer, RegionView const& local,
                      RegionView const& remote, TransferOptions const& opts,
                      RequestPtr* out) = 0;
  virtual Status write(Peer* peer, RegionView const& local,
                       RegionView const& remote, TransferOptions const& opts,
                       RequestPtr* out) = 0;
  virtual Status readv(Peer* peer, std::vector<RegionView> const& local,
                       std::vector<RegionView> const& remote,
                       TransferOptions const& opts, RequestPtr* out) = 0;
  virtual Status writev(Peer* peer, std::vector<RegionView> const& local,
                        std::vector<RegionView> const& remote,
                        TransferOptions const& opts, RequestPtr* out) = 0;

  // --- 通知 ---
  // 成功表示对端 Engine 已收进通知队列，不代表对端应用已处理，
  // 也不隐含任何数据传输的完成。
  virtual Status notify(Peer* peer, std::vector<uint8_t> const& payload,
                        RequestPtr* out) = 0;
  virtual Status poll_notifications(uint32_t max_items,
                                    std::vector<Notification>* out) = 0;

  // --- 完成 ---
  virtual Status poll_completions(uint32_t max_items,
                                  std::vector<RequestPtr>* out) = 0;
  // 收取本端作为写目标时的就绪交接凭证。
  virtual Status poll_ready_events(uint32_t max_items,
                                   std::vector<ReadyEventPtr>* out) = 0;

  // 显式 progress 模式下由调用方驱动；线程模式下调用它也是安全的空操作级别开销。
  virtual Status progress() = 0;

  // --- 设备依赖 ---
  virtual Status record_event(DeviceStream* stream, DeviceEventPtr* out) = 0;

  // 停止接收新任务并 drain。超时则保留必要资源并返回未完成状态，
  // 绝不销毁仍可能被 DMA 访问的对象。
  virtual Status close(int64_t timeout_ms) = 0;
};

}  // namespace hux
#endif  // HUX_ENGINE_H
