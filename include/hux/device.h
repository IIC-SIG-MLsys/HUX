// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_DEVICE_H
#define HUX_DEVICE_H

#include <cstdint>
#include <memory>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

// 应用执行队列的句柄。厂商的 cudaStream_t / hipStream_t / cnrtQueue_t 封装在
// DeviceBackend 实现内部，公共头文件不出现这些类型。
//
// 生命周期由调用方负责：stream 必须保持有效，直到相关 GPU 等待与内存访问安全结束。
class DeviceStream {
 public:
  virtual ~DeviceStream() = default;
  virtual DeviceId device() const = 0;
  // 仅供 provider/backend 内部还原厂商句柄，应用不应当解释这个值。
  virtual void* native_handle() const = 0;
};

// 设备事件。注意各厂商语义不同：CUDA 的等待只针对事件已捕获的工作，
// 而 HIP 对尚未记录的事件查询可能直接返回成功。因此一个"还没 record 的 event"
// 不能被当作"将来自动等待"的通用信号——DeviceBackend 必须校验这一点。
class DeviceEvent {
 public:
  virtual ~DeviceEvent() = default;
  virtual DeviceId device() const = 0;
  virtual bool recorded() const = 0;
  virtual Status query(bool* complete) = 0;
  virtual void* native_handle() const = 0;
};

using DeviceStreamPtr = std::shared_ptr<DeviceStream>;
using DeviceEventPtr = std::shared_ptr<DeviceEvent>;

// 设备后端能力。如实表达限制，不能用静默同步或额外拷贝冒充已承诺的能力。
struct DeviceCaps {
  bool supports_stream = false;        // 能接入应用 stream/event
  bool supports_graph_capture = false; // 与 supports_stream 分别标识、分别测试
  bool supports_peer_registration = false;  // 显存可直接 ibv_reg_mr
  bool supports_dmabuf_export = false;      // 海光 DTK 没有，须如实置 false

  // 单次注册的最大字节数，0 表示无已知限制。
  // 寒武纪 MLU 实测约 32 MiB 且随碎片浮动，这类限制必须在这里暴露出来，
  // 否则调用方会按"注册大 pool"的假设写代码，到运行时才失败。
  uint64_t max_registration_bytes = 0;
};

// 设备能力抽象。每个厂商一个实现，全部可独立启用、构建和测试。
class DeviceBackend {
 public:
  virtual ~DeviceBackend() = default;

  virtual DeviceKind kind() const = 0;
  virtual DeviceCaps caps() const = 0;

  // 识别一个指针属于哪个设备、哪种内存。用于校验注册请求与路径选择。
  virtual Status probe_pointer(void const* ptr, DeviceId* dev,
                               MemoryKind* mem) const = 0;

  // 导入应用已有的 stream。不接管其生命周期。
  virtual Status import_stream(void* native_stream, DeviceStreamPtr* out) = 0;

  // 在指定 stream 上记录一个事件，用于表达"此前的工作已完成"。
  virtual Status record_event(DeviceStream* stream, DeviceEventPtr* out) = 0;

  // 让 stream 等待一个事件。异步安装依赖，不阻塞调用线程。
  virtual Status stream_wait_event(DeviceStream* stream, DeviceEvent* ev) = 0;

  // 传输完成后，让目标数据对该设备上的后续 kernel 可见。
  // GPU 显存直写本身不建立与消费 kernel 的执行顺序，这一步不能省。
  virtual Status make_visible(DeviceStream* stream, void* addr,
                              uint64_t bytes) = 0;
};

}  // namespace hux
#endif  // HUX_DEVICE_H
