// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_CONFIG_H
#define HUX_CONFIG_H

#include <cstdint>
#include <string>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

// progress 模式。两种模式共用同一套完成与错误契约：
// 显式模式下也不允许"只有调用 wait 才推进任务"这种依赖偶然调用的行为。
enum class ProgressMode : uint8_t {
  kThread = 0,  // 后台 progress 线程（默认）
  kExplicit,    // 由调用方持续驱动
};

enum class CongestionControl : uint8_t {
  kOff = 0,      // 对照用
  kFixedWindow,  // 对照用，也可作为运行配置
  kAdaptive,
};

// 三个粒度必须分开配置，不能共用一个参数：
//   segment  应用描述的地址范围
//   chunk    调度与限流的粒度
//   wr_batch 一次 doorbell 提交的 WR 数，决定门铃成本
struct EngineConfig {
  DeviceId device;
  ProgressMode progress = ProgressMode::kThread;

  uint32_t qp_per_peer = 1;
  uint64_t chunk_bytes = 1u << 20;
  uint32_t wr_batch = 16;
  uint32_t cq_batch = 16;

  // 提交队列上限。满时返回 kWouldBlock —— 逻辑请求未被接受，没有网络副作用。
  uint32_t max_inflight_requests = 4096;

  CongestionControl cc = CongestionControl::kOff;
  uint64_t cc_window_bytes = 1u << 22;  // kFixedWindow 时生效

  // 通知队列的容量上限，超出时背压。必须为内部 ready/错误消息保留进展资源，
  // 否则数据预算耗尽会连带控制面一起卡死。
  uint32_t notify_queue_depth = 1024;
  uint32_t notify_max_payload = 4096;

  std::string preferred_provider;  // 空表示自动选择

  // 校验参数间的冲突，返回具体原因而不是静默改写。
  Status validate(std::string* reason) const;
};

}  // namespace hux
#endif  // HUX_CONFIG_H
