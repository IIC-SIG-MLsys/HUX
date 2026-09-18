// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_NOTIFICATION_H
#define HUX_NOTIFICATION_H

#include <cstdint>
#include <memory>
#include <vector>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

class DeviceStream;

// 收到的通知。payload 是变长字节，内容由应用定义——对端需要的信息必须显式放进
// payload，不能指望通过 Request::context() 传递（那个只在本地有意义）。
struct Notification {
  PeerId peer = 0;
  Epoch epoch = 0;
  NotificationId id = 0;
  // 若该通知是与某次数据传输关联发布的，这里是对端那次请求的 id；否则为 0。
  RequestId related_request = 0;
  std::vector<uint8_t> payload;
};

// 通知投递状态。断连后无法判定对端是否收到，这种情况必须能与"确定失败"区分开，
// 因为调用方对两者的处置不同（前者不能简单重发）。
enum class DeliveryState : uint8_t {
  kPending = 0,
  kDelivered,     // 对端 Engine 已接收至通知队列（不代表应用已处理）
  kFailed,
  kIndeterminate, // 已发送但随后断链，投递结果不确定
};

char const* to_string(DeliveryState s);

// 写请求到达对端后，对端拿到的交接凭证。
// 接收端必须用它建立自己的本地依赖——发送端的 stream 不代表接收端的 stream。
class ReadyEvent {
 public:
  virtual ~ReadyEvent() = default;

  virtual RequestId request() const = 0;
  virtual PeerId peer() const = 0;
  virtual RegionId region() const = 0;
  virtual Generation generation() const = 0;
  virtual Span span() const = 0;

  // 在消费流上安装"数据已就绪"的依赖后立即返回。
  virtual Status wait_on(DeviceStream* stream) = 0;
};

using ReadyEventPtr = std::shared_ptr<ReadyEvent>;

}  // namespace hux
#endif  // HUX_NOTIFICATION_H
