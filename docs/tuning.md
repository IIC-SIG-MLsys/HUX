# What is worth tuning, measured

Swept between two machines on RoCE v2 — a Cambricon MLU370-X8 host and a
Hygon Z100L host, one ConnectX-5 each — with host memory, 30 iterations per
point, median of a 1 MiB write.

## Queue pairs: the only setting that moved anything

| queue pairs | median | throughput |
| --- | --- | --- |
| 1 | 212.0 us | 39.6 Gb/s |
| 4 | 98.1 us | 85.6 Gb/s |
| 8 | 98.2 us | 85.4 Gb/s |

Four is 2.2× one, and eight is four. The default is now four.

This is worth stating plainly because the same sweep on a loopback shows
nothing: 15.13, 14.94 and 15.27 Gb/s for one, four and sixteen queue pairs.
A conclusion drawn there — that multiple queue pairs do not help — is an
artefact of measuring a path that never leaves the host. It held for as long
as loopback was the only measurement available, and it was wrong.

## Chunk size: no effect worth having

| chunk | median | sub-operations |
| --- | --- | --- |
| 64 KiB | 215.2 us | 1120 |
| 256 KiB | 213.7 us | 280 |
| 1 MiB | 212.7 us | 70 |
| engine default | 212.0 us | 70 |

A sixteen-fold change in how many sub-operations the engine posts moves the
median by 1.5%. Whatever the cost of chunking is, it is not where the time
goes at these sizes, and a caller has nothing to gain from tuning it.

## What this means for automatic tuning

The roadmap asks for an automatic profile compared against hand tuning. On
this fabric there is nothing for it to find: one setting matters, its useful
range is a single step, and that step is now the default. Building a profiler
to discover it would be building a mechanism to rediscover a constant.

That conclusion is specific to what was measured — two hosts, one NIC each,
one flow at a time. Several flows in contention, several NICs, and incast are
where a scheduler's choices would start to matter, and none of them have been
measured here.

## Reading the numbers

Read and write are not symmetric at 1 MiB: 207 us against 99 on four queue
pairs. At 4 MiB they converge, 369 us each, near 91 Gb/s in both directions.
The asymmetry at the smaller size is worth explaining before it is relied on;
it has not been chased down yet.
