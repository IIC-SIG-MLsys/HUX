## What is HUX?

HUX (Heterogeneous Unified eXchange) is a point-to-point transport engine for
heterogeneous accelerators. Applications register the memory they already own
and transfer in place, over one interface for same-host and cross-host paths,
with the transfer tied into the GPU's own execution order.

* **Application-owned memory**
  `register_memory` takes any pointer the application already holds. There is
  no communication buffer to copy through and no allocator to adopt.
* **Batched asynchronous transfers**
  `Engine` / `Peer` / `MemoryRegion` / `Request`, with scalar and vector
  `read`/`write`, batch registration, exportable descriptors and completion
  polling.
* **One interface for local and remote**
  CPU-CPU, CPU-GPU and GPU-GPU over direct access, IPC, native RDMA or UCX,
  selected by device, process and topology, with the choice queryable.
* **GPU stream and event dependencies**
  Transfers hook into the producing and consuming streams without
  synchronizing the whole device and without an extra payload copy.
* **Completion as part of the design**
  Source reuse, transfer completion and target readiness are separate,
  separately provable states.

### Supported devices

Vendor support comes from device backends and transport providers combined;
the public interface binds to no vendor SDK.

| Device | Backend | State |
| --- | --- | --- |
| NVIDIA GPUs | CUDA | planned |
| AMD GPUs / Hygon DCUs | ROCm / DTK | planned |
| Cambricon MLUs | CNRT / Neuware | planned |
| Moore Threads GPUs | MUSA | planned |
| CPU memory | host | planned |

Each backend is independently enabled, built and tested. Capabilities are
reported as they are: where a device caps registration size or lacks DMA-BUF
export, that shows up in `DeviceCaps` rather than as a silent fallback.

### Relation to HMC

HUX replaces HMC's `ConnBuffer`-centred interface, in which a communicator was
bound to one library-owned buffer and every request carried an IP, a port and a
transport type. The vendor memory layer and the RDMA connection setup carry
over; the public API does not. HMC remains available for existing callers.

## Status

Under construction. The public API, the completion contract, the provider
contract, a mock backend and the hardware-free test suite are in place. No real
transport backend is wired up yet; `notify` and the write-side ready handoff
are not implemented.

## Building

Core and the mock backend depend on no GPU or RDMA SDK, so the library builds
and self-tests on a machine without either:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

Backends are opt-in and off by default:

```
-DHUX_ENABLE_RDMA=ON      Native RDMA provider (libibverbs)
-DHUX_ENABLE_UCX=ON       UCX provider
-DHUX_ENABLE_CUDA=ON      NVIDIA
-DHUX_ENABLE_ROCM=ON      AMD / Hygon
-DHUX_ENABLE_NEUWARE=ON   Cambricon
-DHUX_ENABLE_MUSA=ON      Moore Threads
```

Enabling one whose dependency is missing fails at configure time rather than
being skipped, so a build never quietly comes out without the transport it was
asked for.

## Design notes

A few places that are easy to get wrong, and are therefore pinned down by
tests:

* **A CQE is not a completion.** It proves neither that the data is visible to
  kernels on the target device, nor that the other QPs of the same request are
  done. `accepted`, `source_reusable`, `transfer_complete` and `target_ready`
  are four different things.
* **A timeout is not a cancellation.** `wait(timeout)` ending says nothing
  about whether DMA has stopped or the registration still stands.
* **`kWouldBlock` is not a failure.** The request was never accepted and had no
  network side effect.
* **Batches are not atomic.** A failure may have modified part of the target;
  the error carries `may_have_modified_target` and nothing is replayed
  automatically.
* **Export the rkey, not the lkey.** Substituting one for the other happens to
  work where they coincide and breaks silently elsewhere.

## Layout

```
include/hux/      Public API; no vendor SDK types
src/core/         Request state, scheduling, completion aggregation
src/memory/       Region lifetime and registration
src/device/       Per-vendor DeviceBackend
src/transport/    Provider contract and backends (mock / rdma / ucx / ipc)
src/control/      Peer and region metadata, capability negotiation, epochs
tests/            Contract tests and mock fault injection
```

## License

Apache License 2.0.
