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
