// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_REQUEST_H
#define HUX_REQUEST_H

#include <cstdint>
#include <memory>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

class DeviceStream;

// 完成阶段。这是整个库最容易被做错的地方，所以逐条写清楚边界。
//
// 底层 CQE 不等于任何一个阶段：一个 CQE 只说明某个 WR 离开了本地队列并被
// 对端 NIC 确认，它既不证明数据对目标设备上的 kernel 可见，也不证明同一个
// 逻辑请求的其他 QP 上的子操作已经完成。阶段之间必须能分别证明。
enum class Stage : uint8_t {
  // 参数与队列准入成功，Engine 接管该请求。仅此而已——没有任何数据被搬运。
  kAccepted = 0,

  // 本地源数据不再被传输读取，源 stream 可以覆盖它。
  // 只对写方向有意义。达到这个阶段不代表对端能消费数据。
  kSourceReusable,

  // provider 已满足该方向的传输完成条件。注意这不是 target_ready：
  // 对 UCX 需要相应 flush，对 GPU 目标还需要设备可见性处理。
  kTransferComplete,

  // 目标数据完整，已完成该设备要求的可见性处理，并能为后续消费建立正确依赖。
  // 这是数据请求"成功"的唯一标准。不代表数据已经被消费。
  kTargetReady,

  // 请求失败，相关本地 DMA/设备操作已安全结束或被隔离。
  // 远端可能已被部分修改，见 ErrorInfo::may_have_modified_target。
  kFailedSafe,

  // 请求已取消，已提交的操作完成安全 drain，未发布成功 ready。
  // 部分写入的远端数据不能作为完整数据消费。
  kCancelledSafe,
};

char const* to_string(Stage s);

// 请求状态机。数据请求至少覆盖：
//   QUEUED → WAIT_DEPENDENCY → INFLIGHT → WAIT_TARGET_READY → SUCCEEDED
// 附带通知时在 SUCCEEDED 之前插入 WAIT_NOTIFY_ACK。
// 错误或已接受的取消先进入 DRAINING，确认本地资源安全后才进入终态。
enum class RequestState : uint8_t {
  kQueued = 0,
  kWaitDependency,   // 等待 after=[event] 里的前置设备依赖
  kInflight,
  kWaitTargetReady,
  kWaitNotifyAck,
  kDraining,         // 出错或取消后，等待已提交部分安全结束
  kSucceeded,
  kFailed,
  kCancelled,
};

char const* to_string(RequestState s);
bool is_terminal(RequestState s);

// 一次逻辑传输。一个 Request 可能对应多个分段、多个 WR、多条 QP，
// 这些全部属于内部实现，不对调用方暴露。
//
// 生命周期：Request 持有它用到的 Region、连接等对象的引用，直到安全释放为止。
// 因此调用方提前丢弃 Request 不会让底层资源在 DMA 仍在进行时被回收。
class Request {
 public:
  virtual ~Request() = default;

  virtual RequestId id() const = 0;
  virtual RequestState state() const = 0;

  // 查询是否已达到某个阶段。重复调用返回一致结果。
  virtual bool reached(Stage s) const = 0;

  // 非阻塞查询。done 为 true 表示已进入终态。
  virtual Status test(bool* done) = 0;

  // 阻塞等待至终态。
  //
  // 超时只是"本次等待结束"——它不取消请求、不解除注册、更不证明 DMA 已经停止。
  // 想真正终止请求必须显式 cancel() 并等它进入 kCancelledSafe。
  virtual Status wait(int64_t timeout_ms) = 0;

  // 请求停止尚未提交的子操作；已提交的部分继续 drain。
  // 若请求已经成功或 ready 交接已不可撤回，返回 kInvalidArgument 表示取消过迟。
  // 取消不回滚远端数据。
  virtual Status cancel() = 0;

  // 在指定 stream 上安装"等待该请求 target_ready"的依赖，然后立即返回。
  // 它不阻塞调用线程等待网络传输，也不做全设备同步。
  virtual Status wait_on(DeviceStream* stream) = 0;

  // 单独等待源可复用，用于提前复用写请求的源缓冲区。
  virtual Status wait_source_reusable_on(DeviceStream* stream) = 0;

  virtual ErrorInfo const& error() const = 0;

  // 仅用于本地关联，不会被发送到对端；对端需要的信息走显式 notification payload。
  virtual void* context() const = 0;
};

using RequestPtr = std::shared_ptr<Request>;

}  // namespace hux
#endif  // HUX_REQUEST_H
