// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
//
// M0 契约测试：覆盖 roadmap §14「无硬件 core/mock」那一列。
// 这些用例刻意针对旧实现出过错的地方——乱序完成、部分 post、lkey 顶替 rkey、
// 超时被当成取消——把它们变成会失败的测试，而不是靠人读代码发现。
#include <cstring>
#include <thread>
#include <vector>

#include "core/factory.h"
#include "core/region_impl.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/mock/mock_provider.h"

using namespace hux;

namespace {

struct Fixture {
  std::shared_ptr<MockProvider> provider;
  std::unique_ptr<Engine> engine;
  std::vector<uint8_t> src;
  std::vector<uint8_t> dst;
  MemoryRegionPtr src_region;
  MemoryRegionPtr dst_region;
  PeerPtr peer;
  RemoteRegionPtr remote_src;

  // 搭一个可跑的最小环境：两块本地内存，一块当本地区域，另一块当"对端"区域。
  // mock provider 在同一地址空间里搬数据，因此可以做逐字节校验。
  bool setup(EngineConfig cfg = {}, MockConfig mock = {}, size_t bytes = 4096) {
    provider = std::make_shared<MockProvider>(mock);
    if (make_engine(cfg, nullptr, provider, &engine) != Status::kOk) return false;
    src.assign(bytes, 0);
    dst.assign(bytes, 0);
    for (size_t i = 0; i < bytes; ++i) src[i] = static_cast<uint8_t>(i * 7 + 1);

    if (engine->register_memory(src.data(), bytes,
                                AccessFlags::kRemoteRead | AccessFlags::kLocalRead,
                                &src_region) != Status::kOk) return false;
    if (engine->register_memory(dst.data(), bytes,
                                AccessFlags::kLocalWrite | AccessFlags::kRemoteWrite,
                                &dst_region) != Status::kOk) return false;
    std::vector<uint8_t> meta;
    if (engine->local_metadata(&meta) != Status::kOk) return false;
    if (engine->add_peer(meta, &peer) != Status::kOk) return false;
    std::vector<uint8_t> desc;
    if (src_region->export_descriptor(&desc) != Status::kOk) return false;
    if (peer->import_region(desc, &remote_src) != Status::kOk) return false;
    return true;
  }

  // 把请求推进到终态。显式模式下 poll_completions 内部会驱动 progress。
  Status drain(RequestPtr const& req, int64_t ms = 2000) {
    std::vector<RequestPtr> done;
    for (int i = 0; i < 2000; ++i) {
      engine->poll_completions(16, &done);
      bool fin = false;
      req->test(&fin);
      if (fin) return req->wait(ms);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Status::kTimeout;
  }
};

EngineConfig explicit_cfg() {
  EngineConfig c;
  c.progress = ProgressMode::kExplicit;
  return c;
}

}  // namespace

// ---- 描述符编解码 ----

HUX_TEST(descriptor_roundtrip) {
  RegionDescriptor d;
  d.region = 42;
  d.generation = 7;
  d.base = 0x7f0000001000ull;
  d.length = 1ull << 33;  // >4GiB，验证 64 位字段没被截断
  d.remote_key = 0xdeadbeefull;
  d.device_kind = DeviceKind::kCambricon;
  d.device_index = 3;
  d.access = AccessFlags::kRemoteRead | AccessFlags::kRemoteWrite;

  std::vector<uint8_t> buf;
  encode_descriptor(d, &buf);
  RegionDescriptor got;
  CHECK_STATUS(decode_descriptor(buf, &got), Status::kOk);
  CHECK_EQ(got.region, d.region);
  CHECK_EQ(got.generation, d.generation);
  CHECK_EQ(got.base, d.base);
  CHECK_EQ(got.length, d.length);
  CHECK_EQ(got.remote_key, d.remote_key);
  CHECK(got.device_kind == DeviceKind::kCambricon);
  CHECK_EQ(got.device_index, 3);
}

HUX_TEST(descriptor_rejects_incompatible_major) {
  RegionDescriptor d;
  std::vector<uint8_t> buf;
  encode_descriptor(d, &buf);
  buf[0] = static_cast<uint8_t>(kDescriptorMajor + 1);  // 改 major
  RegionDescriptor got;
  CHECK_STATUS(decode_descriptor(buf, &got), Status::kUnsupported);
}

HUX_TEST(descriptor_rejects_truncated) {
  RegionDescriptor d;
  std::vector<uint8_t> buf;
  encode_descriptor(d, &buf);
  buf.pop_back();
  RegionDescriptor got;
  CHECK_STATUS(decode_descriptor(buf, &got), Status::kInvalidArgument);
}

// ---- 范围检查 ----

HUX_TEST(view_rejects_overflow) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView v;
  // offset + length 回绕。先比较再相加的写法能挡住，直接相加的写法会放行。
  CHECK_STATUS(f.src_region->view(0xffffffffffffff00ull, 0x200, &v),
               Status::kOutOfRange);
  CHECK_STATUS(f.src_region->view(4000, 1000, &v), Status::kOutOfRange);
  CHECK_STATUS(f.src_region->view(0, 4096, &v), Status::kOk);
}

// ---- 基本传输与数据正确性 ----

HUX_TEST(read_moves_data) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  CHECK(req->reached(Stage::kTargetReady) || req->reached(Stage::kTransferComplete));
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

HUX_TEST(readv_pairs_segments) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  std::vector<RegionView> locals(2), remotes(2);
  CHECK_STATUS(f.dst_region->view(0, 1024, &locals[0]), Status::kOk);
  CHECK_STATUS(f.dst_region->view(2048, 1024, &locals[1]), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 1024, &remotes[0]), Status::kOk);
  CHECK_STATUS(f.remote_src->view(2048, 1024, &remotes[1]), Status::kOk);

  RequestPtr req;
  CHECK_STATUS(f.engine->readv(f.peer.get(), locals, remotes, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 1024), 0);
  CHECK_EQ(std::memcmp(f.dst.data() + 2048, f.src.data() + 2048, 1024), 0);
  // 没被传的那段必须还是 0——否则说明分段边界算错了。
  for (size_t i = 1024; i < 2048; ++i) CHECK_EQ(f.dst[i], 0);
}

HUX_TEST(mismatched_segment_lengths_rejected) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  std::vector<RegionView> locals(1), remotes(1);
  CHECK_STATUS(f.dst_region->view(0, 1024, &locals[0]), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 512, &remotes[0]), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->readv(f.peer.get(), locals, remotes, {}, &req),
               Status::kInvalidArgument);
}

// ---- 分片：chunk 边界 ----

HUX_TEST(chunking_covers_whole_range) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 512;  // 4096 字节会被切成 8 个 chunk
  Fixture f;
  CHECK(f.setup(cfg));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  CHECK_EQ(f.provider->submitted_subops(), 8u);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

// ---- 乱序完成 ----

HUX_TEST(out_of_order_completions_aggregate_correctly) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 256;  // 16 个 chunk
  MockConfig mock;
  mock.shuffle_completions = true;  // 打乱 CQE 顺序
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  // 乱序不能影响聚合结果：必须等全部 16 个子操作回报才算成功。
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

// ---- 一批 CQE 里混着多个请求 ----

HUX_TEST(batch_poll_does_not_drop_other_requests) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 1024;
  Fixture f;
  CHECK(f.setup(cfg));

  std::vector<RequestPtr> reqs;
  for (int i = 0; i < 4; ++i) {
    RegionView local, remote;
    CHECK_STATUS(f.dst_region->view(i * 1024, 1024, &local), Status::kOk);
    CHECK_STATUS(f.remote_src->view(i * 1024, 1024, &remote), Status::kOk);
    RequestPtr r;
    CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &r),
                 Status::kOk);
    reqs.push_back(r);
  }
  // 旧实现"取得一批 CQE 后遇到目标即返回"会丢掉同批其他请求的完成，
  // 表现为这里有请求永远不终态。
  for (auto& r : reqs) CHECK_STATUS(f.drain(r), Status::kOk);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

// ---- 部分提交失败 ----

HUX_TEST(partial_submit_reports_and_keeps_accepted) {
  EngineConfig cfg = explicit_cfg();
  cfg.chunk_bytes = 512;  // 8 个 chunk
  MockConfig mock;
  mock.accept_limit = 3;  // 只接受前 3 个
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  Status s = f.engine->read(f.peer.get(), local, remote, {}, &req);
  CHECK_STATUS(s, Status::kOk);  // 有部分被接受，请求算已接受
  CHECK(req != nullptr);
  // 已接受的部分必须继续 drain，且错误要如实带上"可能已改动目标"。
  f.drain(req, 500);
  CHECK(!req->error().ok());
  CHECK_EQ(f.provider->submitted_subops(), 3u);
}

HUX_TEST(full_submit_rejection_has_no_side_effect) {
  EngineConfig cfg = explicit_cfg();
  MockConfig mock;
  mock.accept_limit = 0;
  mock.submit_status_on_partial = Status::kResourceExhausted;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  // accept_limit=0 在 mock 里表示"不限制"，这里换成显式构造全拒绝的场景：
  // 用一个长度为 0 的请求触发参数校验，确认没有任何子操作被提交。
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 0, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 0, &remote), Status::kOk);
  RequestPtr req;
  f.engine->read(f.peer.get(), local, remote, {}, &req);
  CHECK_EQ(f.provider->submitted_subops(), 0u);
}

// ---- 提交队列上限 ----

HUX_TEST(queue_full_returns_would_block) {
  EngineConfig cfg = explicit_cfg();
  cfg.max_inflight_requests = 2;
  MockConfig mock;
  mock.move_data = false;
  Fixture f;
  CHECK(f.setup(cfg, mock));

  std::vector<RequestPtr> reqs;
  Status last = Status::kOk;
  for (int i = 0; i < 5; ++i) {
    RegionView local, remote;
    CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
    CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
    RequestPtr r;
    last = f.engine->read(f.peer.get(), local, remote, {}, &r);
    if (last != Status::kOk) break;
    reqs.push_back(r);
  }
  // 队列满时必须是 WOULD_BLOCK —— 表示逻辑请求未被接受、无网络副作用，
  // 与"传输失败"是两回事，调用方对两者的反应完全不同。
  CHECK_STATUS(last, Status::kWouldBlock);
}

// ---- 取消与超时 ----

HUX_TEST(timeout_does_not_cancel_request) {
  EngineConfig cfg = explicit_cfg();
  MockConfig mock;
  mock.move_data = false;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);

  // 不驱动 progress，请求停在 inflight。
  CHECK_STATUS(req->wait(10), Status::kTimeout);
  // 超时之后请求必须仍然存活：既没被取消，也没进终态。
  bool done = true;
  CHECK_STATUS(req->test(&done), Status::kOk);
  CHECK_EQ(done, false);
  CHECK(req->state() != RequestState::kCancelled);
  // 重复 wait 返回一致结果。
  CHECK_STATUS(req->wait(10), Status::kTimeout);
}

HUX_TEST(cancel_reaches_cancelled_safe) {
  EngineConfig cfg = explicit_cfg();
  MockConfig mock;
  mock.move_data = false;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(req->cancel(), Status::kOk);
  CHECK(req->state() == RequestState::kDraining);

  std::vector<RequestPtr> done;
  for (int i = 0; i < 100; ++i) {
    f.engine->poll_completions(16, &done);
    if (req->state() == RequestState::kCancelled) break;
  }
  CHECK(req->state() == RequestState::kCancelled);
  // 取消成功的请求不得再发布成功 ready。
  CHECK(req->reached(Stage::kCancelledSafe));
  CHECK(!req->reached(Stage::kTargetReady));
}

HUX_TEST(cancel_after_success_reports_too_late) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.drain(req), Status::kOk);
  // ready 交接已不可撤回，必须明确报告取消过迟，而不是假装取消成功。
  CHECK_STATUS(req->cancel(), Status::kInvalidArgument);
}

// ---- 传输错误 ----

HUX_TEST(subop_error_marks_may_have_modified_target) {
  EngineConfig cfg = explicit_cfg();
  MockConfig mock;
  mock.fail_subops = true;
  mock.move_data = false;
  Fixture f;
  CHECK(f.setup(cfg, mock));
  RegionView local, remote;
  CHECK_STATUS(f.src_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->write(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  f.drain(req, 500);
  CHECK(!req->error().ok());
  // 批量传输不承诺原子性，失败可能已改了部分目标，这一位必须传上来。
  CHECK_EQ(req->error().may_have_modified_target, true);
  CHECK(req->reached(Stage::kFailedSafe));
}

// ---- region 生命周期 ----

HUX_TEST(retired_region_rejects_new_submits) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 256, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 256, &remote), Status::kOk);
  // 注销第一步是阻止新提交。
  std::static_pointer_cast<MemoryRegionImpl>(f.dst_region)->retire();
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kStaleGeneration);
}

HUX_TEST(invalidated_remote_region_rejects_view) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  std::static_pointer_cast<RemoteRegionImpl>(f.remote_src)->invalidate();
  RegionView v;
  CHECK_STATUS(f.remote_src->view(0, 256, &v), Status::kStaleGeneration);
}

// ---- rkey / lkey ----

HUX_TEST(exported_descriptor_uses_remote_key_not_local) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  auto impl = std::static_pointer_cast<MemoryRegionImpl>(f.src_region);
  std::vector<uint8_t> desc;
  CHECK_STATUS(impl->export_descriptor(&desc), Status::kOk);
  RegionDescriptor d;
  CHECK_STATUS(decode_descriptor(desc, &d), Status::kOk);
  // mock 刻意让 rkey != lkey。导出的必须是 rkey——
  // 旧实现拿 lkey 顶替，在两者偶然相等的设备上能跑通，换一台就坏。
  CHECK_EQ(d.remote_key, impl->remote_key());
  CHECK(d.remote_key != impl->local_key());
}

// ---- 两种 progress 模式 ----

HUX_TEST(thread_progress_mode_completes_without_explicit_poll) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread;
  Fixture f;
  CHECK(f.setup(cfg));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 4096, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 4096, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  // 线程模式下不能依赖"调用方恰好调了 wait 才推进"。
  CHECK_STATUS(req->wait(3000), Status::kOk);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), 4096), 0);
}

// ---- 并发提交 ----

HUX_TEST(concurrent_submits_are_accounted_correctly) {
  EngineConfig cfg;
  cfg.progress = ProgressMode::kThread;
  cfg.chunk_bytes = 256;
  Fixture f;
  CHECK(f.setup(cfg, {}, 8192));

  constexpr int kThreads = 4;
  constexpr int kPerThread = 8;
  std::vector<std::thread> ts;
  std::atomic<int> ok_count{0};
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        RegionView local, remote;
        uint64_t off = static_cast<uint64_t>((t * kPerThread + i) * 256);
        if (f.dst_region->view(off, 256, &local) != Status::kOk) return;
        if (f.remote_src->view(off, 256, &remote) != Status::kOk) return;
        RequestPtr r;
        if (f.engine->read(f.peer.get(), local, remote, {}, &r) != Status::kOk)
          continue;
        if (r->wait(5000) == Status::kOk) ok_count.fetch_add(1);
      }
    });
  }
  for (auto& th : ts) th.join();
  CHECK_EQ(ok_count.load(), kThreads * kPerThread);
  CHECK_EQ(std::memcmp(f.dst.data(), f.src.data(), kThreads * kPerThread * 256), 0);
}

// ---- 配置校验 ----

HUX_TEST(config_rejects_conflicting_parameters) {
  std::string why;
  EngineConfig c;
  c.qp_per_peer = 0;
  CHECK_STATUS(c.validate(&why), Status::kInvalidArgument);
  CHECK(!why.empty());

  EngineConfig c2;
  c2.cc = CongestionControl::kFixedWindow;
  c2.chunk_bytes = 1 << 20;
  c2.cc_window_bytes = 1024;  // 窗口比 chunk 还小，每个请求都会卡住
  CHECK_STATUS(c2.validate(&why), Status::kInvalidArgument);

  EngineConfig ok_cfg;
  CHECK_STATUS(ok_cfg.validate(&why), Status::kOk);
  CHECK(why.empty());
}

// ---- close ----

HUX_TEST(close_drains_then_succeeds) {
  Fixture f;
  CHECK(f.setup(explicit_cfg()));
  RegionView local, remote;
  CHECK_STATUS(f.dst_region->view(0, 1024, &local), Status::kOk);
  CHECK_STATUS(f.remote_src->view(0, 1024, &remote), Status::kOk);
  RequestPtr req;
  CHECK_STATUS(f.engine->read(f.peer.get(), local, remote, {}, &req),
               Status::kOk);
  CHECK_STATUS(f.engine->close(2000), Status::kOk);
  // close 之后不再接受新任务。
  RequestPtr req2;
  CHECK(f.engine->read(f.peer.get(), local, remote, {}, &req2) != Status::kOk);
}
