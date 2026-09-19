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

Run `tools/probe_registration` on any new device before assuming anything
above transfers to it.

## Transports

| Provider | State | Notes |
| --- | --- | --- |
| Native RDMA | measured | Multiple queue pairs, congestion control, per-queue accounting, NIC affinity |
| Same-process | measured | Copies directly, reports those copies |
| UCX | measured, with a caveat | Works under `UCX_TLS=self,sm`; default transport selection aborts inside the library on this host (see [ucx.md](ucx.md)) |
| IPC between processes on one host | not implemented | Locality is detected and reported; the path itself falls back to RDMA |

## Features against hardware

| Feature | Verified on |
| --- | --- |
| In-place transfer, zero payload copies | RDMA (host, NVIDIA A40, Hygon Z100L and Cambricon MLU370 device memory), UCX, both directions |
| Multiple queue pairs, 1 to 16, balanced accounting | RDMA, loopback |
| Congestion control: off, fixed window, adaptive | RDMA, loopback |
| Device dependencies (`after`), non-blocking submission | RDMA with CUDA on A40 |
| Write-side ready handoff with region and span | RDMA, loopback |
| Acknowledged notifications | RDMA, loopback |
| NIC selection by proximity | RDMA, two NICs on separate NUMA nodes |
| Registration reuse | mock and RDMA |
| Python bindings | mock |

## Not verified

Stated rather than left to be assumed:

- **Cross-machine transfers.** Everything above ran between two processes on
  one host. The path is the same and the loopback figures are not fabric
  numbers.
- **Multiple NICs carrying one transfer.** Selection by proximity is measured;
  striping a request across NICs is not implemented.
- **Sustained operation.** The longest run here is minutes. The roadmap asks
  for 24 hours with zero silent data errors, which has not been attempted.
- **Congestion behaviour under contention.** The controllers are measured on
  synthetic delay samples and on an idle loopback path. Incast, competing
  flows and tail latency under load need several machines.
- **Moore Threads end to end.** Its registration limit is measured and is
  absolute: device memory cannot be registered at any size, so the in-place
  path is impossible there and a staged one is not implemented. Nothing has
  been transferred through this library on that host.

  Hygon Z100L and Cambricon MLU370-X8 have both now been run end to end --
  device memory to device memory, four queue pairs, zero payload copies, both
  directions verified, with the notification and failure paths behaving as on
  NVIDIA.
