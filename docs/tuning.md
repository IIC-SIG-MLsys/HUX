# What is worth tuning, measured

Between two machines on RoCE v2 — a Cambricon MLU370-X8 host and a Hygon
Z100L host, one ConnectX-5 each — host memory, a 1 MiB write, 30 iterations
per point.

## How, and why it is written down

Every point is a fresh pair of processes, and the configurations are run in
**alternating order for three passes**, with the median taken across them.

That is not a formality. The first version of this page reported four queue
pairs being 2.2 times one, from a sweep that ran every one-queue-pair
configuration before every four-queue-pair one. The first group paid a cold
start the second did not, and the difference was attributed to the setting
being swept. In a sequential sweep the configuration and its position in the
run are perfectly confounded, so anything that decays over a run — route and
ARP caches, CPU frequency ramp, page tables, adapter state — lands entirely
on whichever configuration went first.

## Queue pairs: no effect

| queue pairs | runs (alternating) |
| --- | --- |
| 1 | 98.2, 97.8, 97.9 us |
| 4 | 98.1, 98.1, 98.1 us |

Six runs, alternating, spread 0.4%. **The number of queue pairs does not
change anything here**, and the default is one.

This also restores a result that was briefly discarded: the same sweep on a
loopback had shown no effect from more queue pairs, and that was correct. It
was overturned on the strength of the sequential measurement above, which is
the worse of the two mistakes — reporting a wrong number is one thing,
retracting a right one on its authority is another.

## Chunk size: small, real, and not worth tuning

| chunk | runs (alternating) | median |
| --- | --- | --- |
| 64 KiB | 100.5, 100.5, 100.6 | 100.5 us |
| 256 KiB | 98.8, 98.9, 99.1 | 98.9 us |
| 1 MiB | 98.1, 98.3, 97.9 | 98.1 us |

Repeatable to 0.3% and monotone, so the 2.4% between the extremes is a real
effect rather than noise — larger chunks are slightly better. It is still
2.4% across a sixteen-fold change, which is not worth a caller's attention.

## Requests in flight: the link is already full at one

Single flow, 4 MiB writes, 300 per point, three passes in alternating order.

| in flight | median | rate | the three rates |
| --- | --- | --- | --- |
| 1 | 373.4 us | 89.77 Gb/s | 58.1, 89.8, 90.0 |
| 8 | 2927.9 us | 91.66 Gb/s | 91.7, 91.7, 91.6 |
| 32 | 11715.5 us | 91.64 Gb/s | 91.7, 91.6, 91.6 |

The rate stops at 91.6 Gb/s and the latency grows in step with the depth:
373, 2928, 11715 us for 1, 8 and 32. Little's law closes on the same number
from all three rows -- 89.9, 91.7, 91.7 Gb/s -- so beyond the first request
the extra ones are queueing, not moving data any faster. One outstanding
request already reaches 98% of what the link gives.

The first measurement of depth 1 came out at 58.1 Gb/s against 89.8 and 90.0
for the other two: a cold start, caught only because the passes alternate.
Run in order it would have made depth look like the cause.

## Congestion control under contention

Four concurrent flows, 4 MiB writes, 1500 per flow, two passes alternating
the order. Off, a fixed 1 MiB window, and TIMELY.

| | median | slowest/fastest flow |
| --- | --- | --- |
| off | 1763 us | 1.00x |
| fixed 1 MiB | 2427 us | 1.01x |
| TIMELY | 1773 us | 1.01x |

**Fairness is the column that repeats**: six measurements, every one between
1.00 and 1.03. Four flows divide the link evenly **with congestion control
off**, because RC transport already does that in the adapter. There is no
unfairness here for a controller to improve.

Latency separates nothing. The same configuration varied by up to 1.43x
between passes -- off measured 2077 and 1450 us -- which is as large as the
gap between configurations.

**The link was saturated while this ran**, which an earlier version of this
page got wrong. It claimed four flows reached only 36.8 Gb/s of a 100GE
fabric and therefore created no congestion. That figure divided the bytes by
a wall clock that included process startup; the per-flow latency tells the
real story, at 1763 us against 373 us for a single flow -- four flows each
getting about a quarter of a link that is full. The experiment did have
contention. What it does not have is a difference between the controllers.

## What this means for automatic tuning

The roadmap asks for an automatic profile compared against hand tuning. On
this fabric there is nothing for it to find: one setting does nothing at all,
the other moves 2.4% across its whole range, and the better end of that range
is already the default. A profiler here would be a mechanism for
rediscovering two constants.

That conclusion is specific to what was measured — two hosts, one NIC each,
one flow at a time. Several flows in contention, several NICs, and incast are
where a scheduler's choices would start to matter, and none of them have been
measured.

## Reading the numbers

Read and write are not symmetric: a 4 MiB read takes 750 us against 373 us
for a write, 44.7 against 90.0 Gb/s. That is the fabric, not the library. A
read makes the Hygon host's adapter **read** its own memory while a write
makes it **write**, and PCIe read bandwidth is roughly half of write —
a read has to wait for completions where a write is posted. The adapters are
PCIe Gen4 x8 and Gen3 x16 respectively, both around 126 Gb/s in theory, so
the link width is not the limit.

## What a transfer costs in processor time

`hux-bench` reports two CPU figures beside each size: `cores`, which is
processor time over wall time, and `cpu_s/GiB`.

On a single flow at any size, `cores` is 1.00. That is the design and not an
overhead waiting to be removed: both progress modes poll a completion queue,
which trades a core for latency, and an engine that blocked instead would
give the core back and pay an interrupt on every completion. What the figure
establishes is the price -- one core per engine driving one peer, whether it
is moving 65 KiB or 4 MiB.

`cpu_s/GiB` follows from that rather than adding to it. A polling loop's cost
is a function of time, so the cost per byte falls as the link gets faster:
0.50 cpu_s/GiB on a 17 Gb/s path is the same engine as 0.09 on a 91 Gb/s one.
It is comparable between two transports measured on the same path at the same
rate, and meaningless between two rates.
