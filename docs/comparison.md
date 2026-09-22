# Against HMC, on the same link

HMC is the library this one was rebuilt from, so the comparison that matters
is whether the rebuild earned itself. Measured between a Cambricon MLU370-X8
host and a Hygon Z100L host over RoCE v2, one ConnectX-5 each — the same pair
of machines, the same link, in one sitting.

## Making it a fair comparison

Four things were checked before the numbers were taken seriously, because
each one could have produced the result on its own:

**Queue pairs.** HMC connects with one (`local_qps=1, remote_qps=1` in its own
log). HUX was measured at one as well, and separately at four; the two are
indistinguishable on this fabric, so this cannot be the source of a
difference. See [tuning.md](tuning.md).

**Units.** HMC prints `[Network only]`, which would make this an unfair
comparison against an end-to-end number. Reading the code, the label is
misleading: `send_channel_slice_uhm` starts its clock before
`buffer->writeFromGpu(...)` and stops it after `comm->putP2p(...)`, so the
copy into its staging buffer is inside the measurement. Both figures are
therefore end to end.

**Direction.** HMC's client pushes to its server. HUX's `write` is the same
direction over the same link. Its `read` is reported too but is not the
comparison — see the note at the end.

**Order.** Three passes, alternating which library runs first, medians across
them. Running one library's whole sweep before the other's is how a
non-existent 2.2x got measured on this same hardware a day earlier.

## Results

Device memory on both sides. Median of three passes, alternating which
library runs first; the individual runs are in parentheses.

| size | library | median | throughput | runs |
| --- | --- | --- | --- | --- |
| 1 MiB | HMC uhm | 310.0 us | 27.1 Gb/s | 310, 319, 300 |
| | **HUX** | **136.4 us** | **61.5 Gb/s** | 137, 136, 136 |
| 4 MiB | HMC uhm | 814.0 us | 41.2 Gb/s | 814, 819, 804 |
| | **HUX** | **515.8 us** | **65.1 Gb/s** | 520, 516, 516 |
| 16 MiB | HMC uhm | 2484.0 us | 54.0 Gb/s | 2484, 2481, 2489 |
| | **HUX** | **2048.1 us** | **65.5 Gb/s** | 2054, 2045, 2048 |
| 64 MiB | HMC uhm | 9469.0 us | 56.7 Gb/s | 9469, 9425, 9513 |
| | **HUX** | **8246.0 us** | **65.1 Gb/s** | 8278, 8234, 8246 |

2.27x at 1 MiB, narrowing to 1.15x at 64 MiB.

### An earlier version of this page was not a fair comparison

It reported 3.18x to 1.57x. Those numbers came from HUX moving **host**
memory while HMC moved device memory, because the benchmark had no device
backend at all -- it allocated a `std::vector` and registered that. Host
memory is faster on this fabric: 85.9 against 61.5 Gb/s at 1 MiB. The
benchmark takes `--gpu` now and the table above has both sides in device
memory, which is what a GPU application transfers.

Kept here rather than quietly corrected, because the mistake is the kind
that flatters whoever makes it.

## What the shape says

HUX holds about 65 Gb/s from 1 MiB upward, flat. HMC climbs: 27, 41, 54,
56.7. A throughput that rises with transfer size is paying a fixed cost per
transfer, and its size can be read straight off the small end -- at 1 MiB
HMC's own best rate predicts 148 us and it takes 310, so roughly 160 us goes
somewhere that does not depend on how much data is moved.

Two candidates are visible in its code and this measurement does not separate
them: the staging copy into `ConnBuffer`, and the control-message round trip
(`comm->ctrlSend`) after each chunk. A TCP round trip on this pair is of the
right order. Stated as an observation with a candidate explanation, not as a
finding.

By 64 MiB the two are within 15% of each other, both approaching what this
fabric gives for device memory. The architectural difference is still real --
HMC transfers *through* a `ConnBuffer`, a device allocation the application
does not get to use, while HUX registers the application's own memory and the
NIC reads it in place, with `payload_bytes_copied` at 0 to check it -- but on
a large transfer that costs bandwidth rather than latency, and the fabric is
the limit either way.

### What device memory costs here

Worth recording because it is not the library's doing: the same HUX transfer
moves host memory at 85.9-90.2 Gb/s and device memory at 61.5-65.5 Gb/s. Both
sides of that are in-place transfers with no payload copies; the difference
is what the adapters do when the memory belongs to a GPU.

## Where HUX does not win

`read` is reported in the table and is **not** part of the comparison,
because HMC's uhm mode only pushes. It is there because leaving it out would
flatter this library: at 16 and 64 MiB a HUX read is slower than an HMC write
(3108 vs 2484 us, 12414 vs 9469 us).

That is the fabric rather than the library -- a read makes the Hygon host's
adapter read its own memory, and PCIe read bandwidth is roughly half of write
-- and HMC would pay the same on a read if it did them. It has not been
measured on HMC, so it is not claimed.

## UCCL: its same-host path does not run on these GPUs

Attempted on the NVIDIA host, where UCCL is native and already built. Its IPC
path did not run, for a reason that is not a configuration mistake:

```
GPU error engine.cc:1465: peer access is not supported between these two devices
```

UCCL's `connect_local` takes a *remote GPU index* and moves between two
devices, so it needs CUDA peer access. NVIDIA disables that on GeForce parts,
and these are RTX 4090s -- the same class of restriction as their lack of
GPUDirect. Pointing both ends at one GPU instead does not help: `accept_local`
then waits forever, its implementation being a `while (true)` poll over
shared-memory rings with no timeout and no error path.

HUX's same-host path does not need peer access. It maps the peer's allocation
with `cudaIpcOpenMemHandle` and copies across the mapping, which is why it
runs between two processes on a single 4090 -- 300000 verified rounds of it.

That is a difference in what the two can do rather than a measurement, and it
is claimed only for this class of GPU. Where peer access is enabled, UCCL's
path would presumably work and would deserve a real comparison.

## Still to do

A cross-machine comparison against UCCL over RDMA, which is its main path.
What that needs was checked rather than assumed, because an earlier note here
said it needed a particular pair of cards and that was wrong.

It does not need peer access, which is what stopped the same-host
comparison: two machines reach each other through their adapters, not through
the PCIe fabric.

It does need a particular card, and an earlier version of this section said
otherwise on the grounds that `nvidia_peermem` was loaded on both hosts. That
module is necessary and not sufficient. Probed directly here, with the module
loaded, on the same host and the same adapter:

| card | `ibv_reg_mr` on device memory, 4 KiB / 1 MiB / 64 MiB |
| --- | --- |
| RTX 4090 | `EFAULT` at every size |
| A40 | succeeds at every size |

GPUDirect is withheld from the consumer line whatever is installed, which
`docs/support-matrix.md` already recorded from a separate measurement. A
comparison over device memory therefore needs the A40 on each host, not any
spare GPU. A comparison over host memory needs no such thing, and is worth
running on its own terms -- but the pair of hosts is the same either way.

What it does need is two NVIDIA hosts on the RoCE fabric, and there are
exactly two: the machine these measurements were taken on, five RTX 4090s
and an A40, and a second with four 4090s and an A40. The other NVIDIA
machines available -- two with RTX 5090s -- have no adapter on that fabric
at all: no `ibv_devinfo` output, no address on the subnet, no
`nvidia_peermem`. So there is no third host to substitute.

The second of the two carries a sustained load average of 101, which is the
blocker. That is not only a matter of leaving it alone: a benchmark whose
progress thread polls a completion queue measures how often it is scheduled,
and on a machine that oversubscribed it would report the scheduler rather
than the transport. Its root filesystem is also full, at 4.6 GB free, so
building UCCL and HUX there has to go on the data volume.

Neither UCCL nor HUX is installed on it yet. That part is work rather than
waiting, and it is worth doing in advance so the comparison can run whenever
the load drops.
