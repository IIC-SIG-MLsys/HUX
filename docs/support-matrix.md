# Support matrix

Three states are recorded separately, because they mean different things:
**measured** (run on that hardware and verified), **builds** (compiles and its
unit tests pass, but no transfer was run there), and **not supported**.

A capability measured on one model is not claimed for others in the same
family. Where a device was never available, that is said rather than inferred.

## Devices

| Device | Backend | Registration | Streams | State | Measured |
| --- | --- | --- | --- | --- | --- |
| NVIDIA RTX 4090 | CUDA | device memory | yes | measured | 2026-09-18 |
| NVIDIA A40 | CUDA | device memory | yes | measured | 2026-09-18 |
| Hygon Z100L | ROCm / DTK | device memory, all sizes to 512 MiB | yes | measured, transfers end to end | 2026-09-19 |
| Cambricon MLU370-X8 | CNRT | device memory, **256 MiB per process** | yes | measured, transfers end to end | 2026-09-19 |
| Moore Threads S3000 | MUSA | **host memory only** | yes | measured | 2026-09-18 |
| CPU memory | host | host memory | none | measured | 2026-09-18 |
| Ascend | — | — | — | not supported | — |

### What the limits mean

**Cambricon is bounded per process, not per call.** One 256 MiB registration
succeeds and 320 MiB does not, yet four separate 64 MiB regions exhaust the
same quota. A caller checking only a per-call limit will register several
regions and then fail with nothing to point at, which is why `DeviceCaps`
carries both numbers.

**Moore Threads cannot register device memory at all** — 4 KiB is refused the
same as 512 MiB. That backend stages through pinned host memory and reports
`supports_peer_registration = false`. Transfers in place are impossible there,
and the roadmap's zero-copy requirement cannot be met on this device.

**GPUDirect is not a property of the vendor.** On the NVIDIA host measured
here, the A40 supports it over both the peer-memory and DMA-BUF paths while
the RTX 4090s support neither: `CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED` is 0
and `ibv_reg_mr` returns EFAULT at every size. Consumer cards should be
assumed unable until probed.

**Two vendors answer the IPC question differently from NVIDIA.** Hygon's DTK
23.10 returns an error for the unified-addressing attribute and leaves the
value untouched, while exporting handles perfectly well, so the capability is
settled by asking the driver for a handle rather than by believing the query.
Cambricon exports only an allocation's base address -- a handle for base+4096
comes back `CN_MEMORY_ERROR_INVALID_ADDRESS` -- and its handle is 8 bytes, so
a region inside a larger allocation is refused there rather than exported with
an offset the driver gives no way to compute.

**Host memory cannot be exported to another process.** Memory the caller
allocated has no handle another process could map, and `process_vm_readv` is
refused between unrelated processes wherever `kernel.yama.ptrace_scope` is 1 --
the default on all three hosts measured here. The IPC path therefore refuses
host memory rather than staging it through a shared buffer.

Run `tools/probe_registration` on any new device before assuming anything
above transfers to it.

## Transports

| Provider | State | Notes |
| --- | --- | --- |
| Native RDMA | measured | Multiple queue pairs, congestion control, per-queue accounting, NIC affinity, out-of-order placement on mlx5 |
| Same-process | measured | Copies directly, reports those copies |
| UCX | measured, with caveats | In one process, under `UCX_TLS=self,sm` and over RC; both ends must share one provider, and default transport selection aborts inside the library on this host (see [ucx.md](ucx.md)) |
| Choosing among them by where the peer is | measured | One engine holds several in preference order; a peer is reached over the first that suits its location and that the peer also offers ([decision](decisions/0003-path-selection.md)) |
| IPC between processes on one host | measured | Maps the peer's allocation and copies across it, counting the copy; device memory only, and releasing a region waits for the peer to unmap ([decision](decisions/0002-ipc-path.md)) |

## Across machines

Measured between two hosts on RoCE v2, each with a ConnectX-5: a Hygon Z100L
on one and a Cambricon MLU370-X8 on the other. Device memory to device memory,
one and four queue pairs, read and write, both sides verifying the bytes, zero
payload copies, and the notification, handoff and failure paths behaving as
they do on one host. With TIMELY the window settled at 64 KiB and 153
submissions were deferred, which an idle loopback never produces.

One operational point, because it fails in a way that names nothing: the
address in `advertise_ip` selects the port and the GID, so it has to be the
address the kernel actually routes to that peer. One host here has two ports
on the same subnet; naming the other one leaves the queue pairs connected and
every transfer failing with a retry counter that has nothing to say about
which port was wrong. `ip route get <peer>` names the right one.

## Features against hardware

| Feature | Verified on |
| --- | --- |
| In-place transfer, zero payload copies | RDMA (host, NVIDIA A40, Hygon Z100L and Cambricon MLU370 device memory), UCX, both directions |
| Transfer between two machines, and between two vendors | RDMA over RoCE v2, Hygon Z100L to Cambricon MLU370, host and device memory |
| Transfer between two processes on one host | IPC on NVIDIA RTX 4090, Hygon Z100L and Cambricon MLU370-X8, read and write, release waiting on the peer's unmap |
| One engine carrying both a close and a network transport at once | Cambricon MLU370-X8 client with an IPC peer in the next process and an RDMA peer on a Hygon Z100L across RoCE, one registration serving both |
| Multiple queue pairs, 1 to 16, balanced accounting | RDMA, loopback and across two machines; no measured effect on throughput either way (see [tuning](tuning.md)) |
| Congestion control: off, fixed window, adaptive | RDMA, loopback |
| Device dependencies (`after`), non-blocking submission | RDMA with CUDA on A40 |
| Write-side ready handoff with region and span | RDMA, loopback |
| Acknowledged notifications | RDMA, loopback |
| NIC selection by proximity | RDMA, two NICs on separate NUMA nodes |
| Out-of-order placement of reads and writes | RDMA over RoCE v2, ConnectX-5 on PCIe x8 into ConnectX-5 on x4, host memory, 20-minute soak with every byte verified ([comparison](comparison.md#with-out-of-order-placement)) |
| Registration reuse | mock and RDMA |
| Python bindings | mock |

## A defect the soak run found, and what it turned out to be

**Found:** the same-host path lost part of a read and reported success, four
times in 300000 rounds, and the damage was always bytes holding exactly what
this side had written the round before.

**It was not lost data.** Three experiments in sequence: re-reading the bytes
and then re-reading behind a device barrier showed they never arrive, which
ruled out a timing artefact on this side; the sub-operation counters showed
all four posted, completed and accounted for the full payload, which ruled
out a dropped chunk; and finally asking the peer what its own buffer held
answered `neither pattern -- partially refilled`. The peer was announcing
data that had not landed yet. Nothing was ever lost in transfer.

**The hole was in the contract.** `DeviceBackend::copy` returning was taken
by everything above it to mean the bytes were in place -- a provider reports
the sub-operation complete, the engine reports the request complete, a peer
told so reads immediately. The vendor calls do not promise that:
device-to-device performs no host-side synchronization, and a pageable host
source returns once the staging copy is done. An independent probe outside
this library confirmed the distinction: the same pattern with an explicit
`cudaMemcpyHostToDevice` never reproduced in 200 rounds at 64 MiB, while
`cudaMemcpyDefault` did.

`copy` now waits, and says so in its own documentation. `copy_nowait` plus
`settle` exists for a caller that controls every reader until it settles;
the IPC path uses it to pay one synchronization for a batch of
sub-operations instead of one each, and holds the completions back until the
batch has landed.

**What it costs.** A single 4 MiB read goes from 330 us to 1297 us on this
host, because waiting for a copy to land on a GPU shared with another
process means waiting for that process's turn. The 330 us was never real.
End to end the soak run went from 7.3 to 8.5 ms per round, 16% slower, since
the rest of a round dominates. 20000 rounds with no fault where the previous
build failed at round 7.

## Not verified

Stated rather than left to be assumed:

- **Multiple NICs carrying one transfer.** Selection by proximity is measured;
  striping a request across NICs is not implemented.
- **Sustained operation.** The longest run here is minutes. The roadmap asks
  for 24 hours with zero silent data errors, which has not been attempted.
- **Congestion behaviour under contention.** The controllers now run over a
  real fabric, where TIMELY closes its window on measured delay rather than on
  the zero one loopback reports. The fabric was idle. Incast, competing flows
  and tail latency under load still need several machines at once.
- **Moore Threads end to end.** Its registration limit is measured and is
  absolute: device memory cannot be registered at any size, so the in-place
  path is impossible there and a staged one is not implemented. Nothing has
  been transferred through this library on that host.

  Hygon Z100L and Cambricon MLU370-X8 have both now been run end to end --
  device memory to device memory, four queue pairs, zero payload copies, both
  directions verified, with the notification and failure paths behaving as on
  NVIDIA.
