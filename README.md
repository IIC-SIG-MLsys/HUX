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

| Device | Backend | RDMA registration | Notes |
| --- | --- | --- | --- |
| NVIDIA GPUs | CUDA | device memory | RTX 4090, A40 |
| Hygon DCUs / AMD GPUs | ROCm / DTK | device memory | Z100L; DTK exports no DMA-BUF |
| Cambricon MLUs | CNRT / Neuware | device memory, 256 MiB per process | MLU370-X8 |
| Moore Threads GPUs | MUSA | **host memory only** | S3000; device memory is refused |
| CPU memory | host | host memory | no execution queue |

Transfers between engines in the same process take a local path that copies
directly, reporting those copies rather than claiming to be zero-copy. It
holds the same contract as the network path: a peer's address is a key plus an
offset resolved through a registry, never a pointer taken on trust, so code
written against it keeps working when the peer moves to another host.

Each backend is independently enabled, built and tested on its own hardware.
Capabilities are reported as measured, not as hoped: Cambricon registration is
bounded per process rather than per call, and Moore Threads device memory
cannot be registered at any size, so that backend stages through pinned host
memory and reports `supports_peer_registration = false`. `tools/` holds the
probe those numbers come from; run it first on any new device.

Moving an application from HMC: [docs/migration.md](docs/migration.md), with
a working example in [apps/transfer_example.cpp](apps/transfer_example.cpp).

### Relation to HMC

HUX replaces HMC's `ConnBuffer`-centred interface, in which a communicator was
bound to one library-owned buffer and every request carried an IP, a port and a
transport type. The vendor memory layer and the RDMA connection setup carry
over; the public API does not. HMC remains available for existing callers.

## Status

Under construction. Application-owned memory now transfers in place over real
RDMA, host to host and device to device, verified byte for byte in both
directions with no buffer owned by the library anywhere on the path.

In place: the public API, the completion contract, the provider contract, a
single-QP RDMA provider, device dependencies, the write-side ready handoff,
an engine-level control channel, acknowledged notifications, copy accounting,
multiple queue pairs with per-queue accounting, three congestion control
configurations, byte-quantum scheduling between requests, registration reuse,
peer and region invalidation, Python bindings, NIC topology discovery, a mock
backend,
device backends for all five targets above, and a test suite that runs without
hardware. Not yet: Python bindings, multiple NICs, and paths other than RDMA.

Control traffic runs on its own channel rather than sharing the data path's
budget, so a stalled transfer cannot starve the message that would explain
it.

The RDMA provider will be written rather than delegated to UCCL; the measured
reasoning is in [docs/decisions/0001-rdma-provider.md](docs/decisions/0001-rdma-provider.md).

## Building

Core and the mock backend depend on no GPU or RDMA SDK, so the library builds
and self-tests on a machine without either:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

Python bindings are opt-in as well, and need nanobind:

```bash
pip install nanobind
cmake -S . -B build -DHUX_BUILD_PYTHON=ON
cmake --build build -j
PYTHONPATH=build python -c "import hux; print(hux.make_mock_engine().describe())"
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

## What CI checks

Everything in the pipeline runs on a machine with no GPU, no RDMA device and
no vendor SDK: the core and mock suite (94 cases, none of them skipped), the
Python bindings, formatting, and that enabling a backend whose dependency is
absent fails at configure time rather than producing a build without the
transport it was asked for.

Hardware paths are exercised on the machines that have the hardware —
`tests/manual/` for transfers and sweeps, `tools/` for what a new device can
do. A pipeline that could only run there would check nothing on most changes.

## Formatting

```bash
./format.sh           # rewrite in place
./format.sh --check   # report and fail, for CI
```

Google style with left-aligned pointers and east const, matching UCCL, so
code moving between the two does not churn on formatting alone. One
clang-format version is pinned rather than a range: versions disagree on
details, and allowing several would mean whoever ran last decides the diff.

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
* **Reuse of a registration has to be exact.** A range that only partly
  overlaps an existing one covers bytes the hardware was never told about, and
  the transfer that follows fails far from the registration that caused it.
* **Export the rkey, not the lkey.** Substituting one for the other happens to
  work where they coincide and breaks silently elsewhere.
* **One queue pair completing says nothing about the others.** Ordering holds
  within a queue pair, not across them, so arrival is announced only after
  every sub-operation has completed.
* **Every queue pair used needs its own signalling anchor.** Completions are
  the only way posted entries are reclaimed, and a queue left without one
  stalls with nothing to wait for.
* **Out of budget is not a failure.** The remainder is offered again later;
  dropping it would lose data the caller believes is on its way.
* **A registration holds the buffer it covers.** `PyObject_GetBuffer` keeps a
  reference to the exporting object, so a caller may register a tensor and
  drop it. Nothing else would keep the memory alive, and the failure would be
  a crash with no traceback.
* **Every blocking call releases the GIL.** One that does not freezes every
  other thread in the interpreter, including whichever one would have polled
  for the completion being waited on.
* **A configuration report has to cover both halves.** Queue pairs,
  signalling and the congestion controller belong to the provider; a report
  built from the engine's settings alone describes a configuration nobody is
  running.
* **The delay a controller reacts to is not a network round trip.** It is
  measured from the post to its completion, so it includes serialization,
  queueing at the NIC and host, and polling delay. The algorithm works on it;
  results must not be labelled as RTT.

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
