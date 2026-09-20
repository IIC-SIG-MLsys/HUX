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

Median of three passes; the individual runs are in parentheses.

| size | library | median | throughput | runs |
| --- | --- | --- | --- | --- |
| 1 MiB | HMC uhm | 310.0 us | 27.1 Gb/s | 317, 310, 305 |
| | **HUX** | **97.6 us** | **85.9 Gb/s** | 98, 98, 98 |
| 4 MiB | HMC uhm | 809.0 us | 41.5 Gb/s | 809, 810, 801 |
| | **HUX** | **373.1 us** | **89.9 Gb/s** | 372, 373, 374 |
| 16 MiB | HMC uhm | 2483.0 us | 54.1 Gb/s | 2465, 2504, 2483 |
| | **HUX** | **1487.6 us** | **90.2 Gb/s** | 1489, 1487, 1488 |
| 64 MiB | HMC uhm | 9417.0 us | 57.0 Gb/s | 9468, 9417, 9381 |
| | **HUX** | **5986.5 us** | **89.7 Gb/s** | 5984, 5988, 5986 |

3.18x at 1 MiB, narrowing to 1.57x at 64 MiB.

## What the shape says

The multiples are less informative than the curves. HUX holds about 90 Gb/s
from 1 MiB upward — near the 100GE line rate, and flat. HMC climbs: 27, then
41, then 54, then 57 Gb/s. A library whose throughput rises with transfer
size is paying a fixed cost per transfer, and the size of that cost can be
read off directly: at 1 MiB, HMC's own best rate would predict 147 us, and it
takes 310. Something around 160 us is spent on each transfer regardless of
how much data it carries.

Two candidates are visible in its code, and this measurement does not
separate them: the staging copy into `ConnBuffer`, and the control-message
round trip (`comm->ctrlSend`) that follows each chunk. The copy is device to
device and should be cheap; a TCP round trip on this pair is of the right
order. Stated as an observation with a candidate explanation, not as a
finding.

The architectural difference behind it is real either way. HMC transfers
*through* a `ConnBuffer` — allocated with `allocatePeerableBuffer`, so device
memory, and a real allocation the application does not get to use. HUX
registers the application's own memory and the NIC reads it in place;
`payload_bytes_copied` is 0 for this path, and that counter is checkable
where a claim in a README is not.

## Where HUX does not win

`read` is reported above and is **not** part of the comparison, because HMC's
uhm mode only pushes. It is in the table because leaving it out would flatter
this library: at 16 and 64 MiB a HUX read is slower than an HMC write (2853
vs 2483 us, 10217 vs 9417 us).

That is the fabric rather than the library — a read makes the Hygon host's
adapter read its own memory, and PCIe read bandwidth is roughly half of write
— and HMC would pay the same on a read if it did them. But it has not been
measured on HMC, so it is not claimed.

## Not done

UCCL. It is not installed on either of these two machines, and its benchmark
needs torch; the Hygon host runs an old DTK where it may not build at all.
The workable path is a different pair — two NVIDIA hosts, where UCCL is
native — and that has not been set up.
