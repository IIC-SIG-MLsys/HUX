/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_CONFIG_H
#define HUX_CONFIG_H

#include <cstdint>
#include <string>

#include "hux/status.h"
#include "hux/types.h"

namespace hux {

/* Both modes share one completion and error contract. Explicit mode must not
 * depend on the caller happening to call wait() for progress to be made. */
enum class ProgressMode : uint8_t {
  kThread = 0,
  kExplicit,
};

enum class CongestionControl : uint8_t {
  kOff = 0,
  kFixedWindow,
  kAdaptive,
};

/* Three granularities, deliberately not sharing one knob: application segments
 * describe address ranges, chunks drive scheduling and pacing, and wr_batch
 * sets the doorbell cost. */
struct EngineConfig {
  DeviceId device;
  ProgressMode progress = ProgressMode::kThread;

  /* Requested, not authoritative. Queue pairs are created by the provider and
   * its own setting decides; this one is what the caller asked for. The
   * report from Engine::describe() shows both, so the difference is visible
   * rather than something to be inferred. */
  uint32_t qp_per_peer = 1;
  uint64_t chunk_bytes = 1u << 20;
  uint32_t wr_batch = 16;
  uint32_t cq_batch = 16;

  /* Submission queue bound; kWouldBlock above it. */
  uint32_t max_inflight_requests = 4096;

  /* Bytes a single request may submit before the scheduler moves on. Without
   * a bound, a large request submits everything it has and a small one behind
   * it waits for the whole transfer -- a delay that does not show up in
   * aggregate throughput. Rounded up to one chunk, since yielding mid-chunk
   * would split work the provider takes as a unit. */
  uint64_t scheduler_quantum_bytes = 1u << 20;

  /* How many registrations may be kept for reuse after their handles are
   * released. Registering is expensive enough to be worth caching, and
   * unbounded caching would hold hardware resources the process has no use
   * for. Zero disables reuse entirely. */
  uint32_t registration_cache_entries = 64;

  /* Also requested rather than authoritative: the controller is installed on
   * the provider, and a controller supplied there takes precedence. */
  CongestionControl cc = CongestionControl::kOff;
  uint64_t cc_window_bytes = 1u << 22;

  /* Notifications back-pressure past this depth. Internal ready and error
   * messages keep their own resources so a data stall cannot block them. */
  uint32_t notify_queue_depth = 1024;
  uint32_t notify_max_payload = 4096;

  std::string preferred_provider; /* Empty selects automatically. */

  /* Reports conflicting parameters instead of silently rewriting them. */
  Status validate(std::string* reason) const;
};

}  // namespace hux
#endif  // HUX_CONFIG_H
