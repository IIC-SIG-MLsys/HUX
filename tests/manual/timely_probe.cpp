/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * What the congestion controller actually allows, asked of the controller
 * rather than inferred from a benchmark.
 *
 * A run that moves 2.97 Gb/s on a path that carries 91 has several possible
 * causes -- the window, the pacing, the rate, the engine above -- and a
 * benchmark cannot tell them apart. This drives the controller directly with
 * a fixed pattern and a simulated path, and reports which verdict it gave
 * and what its rate reached. That is how the ramp was found: with megabyte
 * operations almost every tick came back kPaced and the rate stopped at 3.47
 * Gb/s, while the window never once refused anything.
 *
 * Build: g++ -O2 -std=c++20 -Isrc -Iinclude -o timely_probe \
 *          tests/manual/timely_probe.cpp src/transport/cc/controller.cpp */
#include <cstdio>
#include <deque>

#include "transport/cc/controller.h"
using namespace hux;

int main(int argc, char** argv) {
  uint64_t const chunk =
      argc > 1 ? std::strtoull(argv[1], nullptr, 10) : (1u << 20);
  uint64_t const small = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 16384;
  bool const mixed = argc > 3 ? std::atoi(argv[3]) != 0 : true;

  auto cc = make_cc_timely();
  auto t = CcClock::now();
  /* A path with a 15 us base delay and 91 Gb/s of serialization. */
  auto delay_for = [&](uint64_t bytes) {
    return std::chrono::nanoseconds(15000 + (int64_t)(bytes * 8.0 / 91.0));
  };
  struct Op {
    CcTime done;
    uint64_t bytes;
    std::chrono::nanoseconds rtt;
  };
  std::deque<Op> live;
  uint64_t sent = 0, paced = 0, over = 0, allowed = 0;
  auto const start = t;
  auto const end = t + std::chrono::milliseconds(200);

  while (t < end) {
    while (!live.empty() && live.front().done <= t) {
      cc->on_feedback(CcDirection::kWrite, live.front().bytes, live.front().rtt,
                      t);
      live.pop_front();
    }
    uint64_t const bytes = mixed ? ((sent % 2 == 0) ? small : chunk) : chunk;
    CcVerdict v = cc->allow(CcDirection::kWrite, bytes, t);
    if (v == CcVerdict::kAllowed) {
      cc->on_post(CcDirection::kWrite, bytes, t);
      auto d = delay_for(bytes);
      live.push_back({t + d, bytes, d});
      sent += bytes ? 1 : 0;
      allowed += bytes;
    } else if (v == CcVerdict::kPaced) {
      ++paced;
    } else {
      ++over;
    }
    t += std::chrono::microseconds(1);
  }
  double const secs = std::chrono::duration<double>(end - start).count();
  std::printf(
      "chunk=%-9llu mixed=%d  admitted %.2f Gb/s   paced=%llu overbudget=%llu"
      "  window=%llu  rate=%.2f Gb/s  inflight=%llu\n",
      (unsigned long long)chunk, (int)mixed, allowed * 8.0 / (secs * 1e9),
      (unsigned long long)paced, (unsigned long long)over,
      (unsigned long long)cc->window_bytes(CcDirection::kWrite),
      cc->rate_bytes_per_sec(CcDirection::kWrite) * 8.0 / 1e9,
      (unsigned long long)cc->inflight_bytes(CcDirection::kWrite));
  return 0;
}
