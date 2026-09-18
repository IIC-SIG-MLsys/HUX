# HUX — Heterogeneous Unified eXchange

面向异构 GPU 的点对点通信引擎。目标是让应用把**自己已有的显存**直接注册并传输，
同机与跨机使用同一套接口，并把传输接进 GPU 的执行顺序。

本仓库按 `transport_roadmap.md` 实施。它不是 HMC 的分支：HMC 以 `ConnBuffer`
为中心的接口按路线图是要退役的，这里从空目录起步，只按需搬运其中经过验证的部分
（厂商内存适配、RDMA 建连知识、以及已知问题对应的回归用例）。

## 当前状态

**M0 契约与基线** —— 已完成。公共 API、完成契约、provider 契约、mock 后端与
无硬件测试可用。尚未接入任何真实传输后端，`notify` 与写方向的 ready 交接尚未实现。

| 里程碑 | 状态 |
|---|---|
| M0 契约与基线 | 完成 |
| M0.5 provider 选型决断 | 未开始 |
| M1 单连接直传闭环 | 未开始 |
| M2 并行与拥塞控制 | 未开始 |
| M3 设备与路径能力 | 未开始 |
| M4 绑定与运行保障 | 未开始 |
| M5 验收交付 | 未开始 |

## 构建

core 与 mock 不依赖任何 GPU 或 RDMA SDK，在没有硬件的机器上可以完整构建并自测：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

各后端独立开关，默认全部关闭：

```
-DHUX_ENABLE_RDMA=ON      Native RDMA provider (libibverbs)
-DHUX_ENABLE_UCX=ON       UCX provider
-DHUX_ENABLE_CUDA=ON      NVIDIA
-DHUX_ENABLE_ROCM=ON      AMD / 海光 DCU
-DHUX_ENABLE_NEUWARE=ON   寒武纪
-DHUX_ENABLE_MUSA=ON      摩尔线程
```

**启用了却找不到依赖会在配置阶段直接失败**，不会静默跳过——否则会构建出一个
自以为带 RDMA 的库。

## 设计要点

几个容易做错、因此在代码和测试里被反复盯住的地方：

- **完成阶段必须能分别证明。** 一个 CQE 既不证明数据对目标设备的 kernel 可见，
  也不证明同一逻辑请求在其他 QP 上的子操作已完成。`accepted` / `source_reusable` /
  `transfer_complete` / `target_ready` 是四件不同的事。
- **超时不是取消。** `wait(timeout)` 超时只表示本次等待结束，请求仍在途，
  DMA 未必停止，注册也没有解除。
- **`WOULD_BLOCK` 不是失败。** 它表示逻辑请求未被接受、没有任何网络副作用。
- **批量传输不承诺原子性。** 失败可能已经改了部分目标，错误里带
  `may_have_modified_target`，不自动重放结果不明的写入。
- **导出给对端的必须是 rkey。** 拿 lkey 顶替在两者偶然相等的设备上能跑通，换一台就坏。
- **设备限制如实声明。** 寒武纪的注册上限、海光的 DMA-BUF 缺失通过 capability 暴露，
  不用静默降级掩盖。

## 目录

```
include/hux/      公共 API，不出现任何厂商 SDK 类型
src/core/         请求状态、调度、完成聚合
src/memory/       区域生命周期与注册
src/device/       各厂商 DeviceBackend
src/transport/    provider 契约与各后端实现（mock / rdma / ucx / ipc）
src/control/      peer 与 region 元数据、能力协商、epoch
tests/            契约测试与 mock 故障注入
```

## 许可

Apache License 2.0。
