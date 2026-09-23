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

## Against UCX and UCCL, across machines

Host memory, one adapter each end, between the two NVIDIA hosts on the RoCE
fabric. The sender's adapter sits on PCIe ×8 and the receiver's on ×4, so
the path tops out near 24 Gb/s and every 1 MiB write the sender pushes at its
own line rate has to be absorbed by a slower receiver -- the path is
congested by construction, and the adapters' congestion machinery (ECN and
DCQCN) is active on it. Both hosts carried other people's work throughout:
load 65-84 on the sender's 64 cores and 104-124 on the receiver's. That is the
condition the earlier UCCL measurement was made under, and the load is
recorded with every run.

UCX is 1.20.0, built once with its release configuration and the same binary
run on both ends. Its arm is `ucx-bench` (`-DHUX_BUILD_UCX_BENCH=ON`), a plain
UCP program linked against nothing of HUX, with hux-bench's definitions
throughout: latency from issue to known-complete, percentiles as the sorted
sample at floor(q(n-1)), warm-up and depth meaning the same thing. Before it
was trusted it was held against UCX's own `ucx_perftest` on the same path:
1 MiB put 343.8 us and 24.4 Gb/s against perftest's 348.5 us and 23.3 Gb/s;
16 KiB, 8.8 against 9.0 us. It writes a pattern to the peer and reads it back
at the end of every run.

A UCP put completes when its source may be reused, which for a short put is
before the bytes leave the host. So a put is timed to a flush of its
endpoint, UCP's own way to learn the bytes arrived and the guarantee a HUX
write gives. UCX's tagged send is a separate arm: above the rendezvous
threshold it cannot complete until the receiver has progressed.

### One-sided writes, 1 MiB, one outstanding

Five passes of 3000, the arms rotated through the order:

| | p50 us | p90 us | p99 us | p99 of each pass |
| --- | ---: | ---: | ---: | --- |
| UCX put | 343.6 | 351.7 | **359.2** | 358 353 360 359 365 |
| HUX write | 362.6 | 373.0 | 1534.3 | 1554 1461 1534 1560 1504 |
| UCCL write | 366.0 | 1513.0 | 3352.4 | 3212 4010 3184 3352 3372 |

UCX's tagged send was in this rotation too, but its row is withdrawn: the
server then reposted only receives that finished later, not ones that
finished in place, so its pool of posted receives could shrink and a sender
wait for one that was not there. Measured again with that fixed, six passes
alternating with the put, the receiver at load 134:

| | p50 us | p90 us | p99 us | p99 of each pass |
| --- | ---: | ---: | ---: | --- |
| UCX tagged send | 356.1 | 585.4 | 2285.9 | 1655 4104 1886 2686 1174 3772 |
| UCX put | 349.4 | 354.9 | 360.9 | 363 360 361 363 361 359 |

Against UCCL, HUX keeps the tail: 4.1x lower at p90 and 2.2x at p99. The 54x
reported earlier came from 600 samples, in which UCCL's p99 was 21 ms; with
3000 its p99 sits at 3.2-4.0 ms in every pass.

What this does not test is UCCL's own claim about congestion. That belongs to
UCCL-collective, its replacement for NCCL, measured against NCCL, and comes
from spraying packets in software across up to 256 network paths with
latency-based or receiver-driven congestion control and selective-repeat
recovery -- a remedy for flows colliding on one path through a multi-path
fabric. The arm here is UCCL-P2P, the point-to-point engine comparable to
HUX, whose own published comparison puts it level with NIXL over UCX. And
this path gives spraying nothing to work with: one adapter each end, since
the second is unreachable on these hosts (which also turns off UCCL-P2P's
default use of several adapters), so one path, with the bottleneck in the
receiving host's PCIe rather than in the network. So UCCL trailing UCX here
says nothing either way about congestion in a fabric.

Against UCX's put it does not: UCX's p99 is 4.3x lower and its median 5%.

The tails that blow up belong to the protocols that need the receiving
host's CPU -- UCX's tagged rendezvous, 1.2-4.1 ms at p99 in every pass while
the put beside it stays at 359-363 us, and UCCL, whose receiver advertises
the slots written into. UCX's put needs nothing from the receiver and has
the tightest distribution of any arm. So
the earlier explanation of the UCCL gap, that a one-sided write does not wait
for a loaded receiver, holds. What it does not explain is why HUX's write,
which is just as one-sided, has a tail at all.

### Why: the adapter drops HUX's bursts and not UCX's

The adapters' own counters, read before and after one run of each:

| | sender: `packet_seq_err` | sender: `rp_cnp_handled` | receiver: `out_of_sequence` | receiver: `rx_write_requests` |
| --- | ---: | ---: | ---: | ---: |
| HUX | 990 | 2022 | 990 | 3005 |
| UCX put | 0 | 0 | 0 | 3,077,122 |
| UCX put, standard ordering | 1090 | 2208 | 1090 | 3007 |

A HUX write is one 1 MiB message. The sender puts its 1024 packets on the
wire back to back, the ×4 receiver cannot drain them as fast, and about one
write in three loses a packet: go-back-N retransmission follows, and a DCQCN
rate cut. That is the tail. UCX lost nothing, and the receiver counted 1024
write requests per put -- every packet placed on its own.

The difference is one adapter feature. Where the device supports it, UCX
creates its RC queue pairs with out-of-order data placement enabled (mlx5
DEVX `dp_ordering_ooo`). Forced back to the standard IBTA ordering
(`UCX_RC_MLX5_AR_ENABLE=n UCX_RC_MLX5_DDP_ENABLE=n`), which is what HUX's
portable verbs queue pairs use, UCX lost 1090 and 1013 packets in two runs
and showed the same tail: p99 383 and 1667 us.

What drops the packets is the mismatch in the hardware, not the load on the
hosts. The sending host has a second adapter, on PCIe ×4 like the receiver's.
Same hosts, same load, same receiver, only the sending adapter changed, two
passes each:

| | sender -> receiver | write p99 us | packets lost |
| --- | --- | ---: | ---: |
| HUX | ×8 -> ×4 | 1483, 1507 | 947, 1104 |
| HUX | ×4 -> ×4 | 392, 403 | 0, 0 |
| UCX, standard ordering | ×8 -> ×4 | 1465, 368 | 1040, 1076 |
| UCX, standard ordering | ×4 -> ×4 | 370, 374 | 0, 0 |

Rate-matched, neither loses a packet and neither has the tail. So the tail
needs a sender faster than its receiver -- common enough in practice, with
adapters of different generations or slots of different widths, which is
why the feature is still worth having -- and on matched hardware HUX's write
tail is as clean as UCX's. The load on the hosts is left to account for the
other two tails, UCX's tagged rendezvous and UCCL's, whose protocols need
the receiving CPU to run; a shared host cannot be unloaded to test that, so
it is inferred rather than measured.

Ruled out on the way, each by measurement rather than argument: the ready
handoff (its send never took more than 37 us, and a build that skips it kept
the tail), the receiving side's progress thread, congestion control (off),
signalling (every batch ends signalled), relaxed ordering (UCX identical with
it on, off and auto -- both hosts are Intel), and the traffic class (UCX
identical at 0 and 106). Chunking HUX's write at the 1 KiB MTU does remove the
drops -- 0 out of order -- but posts 1024 work requests per MiB and halves the
throughput (median 655-728 us), so that is not the fix.

So, attributed: with the same ordering semantics HUX and UCX lose the same
packets and show the same tail, and UCX's median is about 5% ahead. UCX's
default advantage on this path is an adapter feature HUX does not yet turn
on. rdma-core exposes it as `MLX5DV_QP_CREATE_OOO_DP`; it has to be set on
both ends, and it relaxes the order in which a message's bytes land. HUX's
completion contract does not depend on that order, but two things would have
to be settled first: an application polling the last byte of a buffer would
break, and HUX's inference that a signalled completion covers the unsignalled
ones before it would need to be shown to still hold.

### Short writes behind long ones

16 KiB writes interleaved with 4 MiB ones, eight outstanding, 2000 of each,
three passes:

| | short, alone | short, mixed p50 | short, mixed p99 | long, mixed p50 | Gb/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| HUX, no control | 47.9 | 5890.6 | 6232.6 | 6291.8 | 22.46 |
| HUX, 1 MiB window | 38.5 | **23.4** | **40.2** | 10963.3 | 21.73 |
| UCX | 39.6 | 5653.1 | 5838.8 | 5653.4 | 23.94 |
| UCX, standard ordering | 41.0 | 6041.3 | 6323.2 | 6042.1 | 22.31 |
| UCX, second endpoint for short | 39.6 | **24.0** | **39.7** | 11085.4 | 24.24 |

Without a mitigation both are blocked alike, the short write waiting behind
about 5.7 ms of long ones. HUX's window takes it to 23 us on one connection;
UCX gets the same by giving short messages an endpoint of their own. So what
HUX offers here is that the application does not have to split its traffic
across connections, not that it goes faster -- UCX moved 12% more over the
same mix with its second endpoint, again from losing no packets.

### Still to do

Device memory. GPUDirect is withheld from the RTX 4090 whatever is
installed -- `ibv_reg_mr` on its memory returns `EFAULT` at every size, with
`nvidia_peermem` loaded, while the A40 on the same host and adapter succeeds
-- so a device-memory comparison needs the A40 on each host, and one of the
two has been occupied by someone else's work.
