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

## Congestion control where it can matter: a short write behind a long one

A uniform stream gives a controller nothing to do -- every request is the
same size, so nobody is stuck behind anybody, and fairness is already 1.00x
because RC transport provides it. Interleaving 16 KiB and 4 MiB writes at
depth 8 puts about four large transfers ahead of each small one, which is
the harm a controller exists to prevent.

Four passes rotating the order so each configuration runs in each position,
2000 transfers a point, each size also measured alone for reference. The
receiver reads its control channel, so the notice each write produces is
delivered rather than dropped -- an earlier version of this table was taken
against one that did not, and every small-write figure in it was about 3 us
lower than the truth.

| | small p50 | small p99 | large p50 | rate | behind |
| --- | --- | --- | --- | --- | --- |
| off | 1297.5 us | 1484.2 | 1485.9 | **91.14 Gb/s** | 36.6x |
| fixed 1 MiB window | **11.2 us** | **12.7** | 2992.9 | 81.67 | 0.4x |
| TIMELY, the paper's increase | 257.1 | 3007.4 | 5046.4 | 43.58 | 7.9x |
| TIMELY, increase scaled to this fabric | 11.6 | 1121.2 | 2973.8 | 80.84 | 0.4x |

"behind" is how much longer a small write takes with large ones ahead of it
than it takes alone at the same depth.

**A window costs 10% of the throughput and takes 116x off the small write's
latency.** That is the trade, and it is a large one: without it a 16 KiB
write waits 1.3 ms behind traffic it has nothing to do with.

**The adaptive controller does not beat the fixed window.** Scaled to this
fabric it reaches the same median -- 11.6 us against 11.2 -- and its tail is
88 times worse, 1121.2 against 12.7. Left at the paper's increase it costs
half the throughput as well. CC-02 asks for an adaptive controller that
meets a target; measured against the simplest thing that has a window at
all, it does not earn its complexity here.

The medians are over four passes and one of them is thrown out by taking
them. The first configuration of a run keeps coming back disturbed -- here
"off" measured 39.26 Gb/s in pass 1 against 91.05, 91.23 and 91.30 in the
other three, and two minutes of settling beforehand did not prevent it. A
median of four is why the table can be read anyway, and a mean would not
be.

These numbers replace an earlier run in which TIMELY managed 2.97 Gb/s and a
head-of-line factor of 818. That was a defect rather than a result: its rate
was raised once per completion, so large operations produced few completions
and it never climbed. See below.

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

## Ordering a transfer against the caller's own GPU work

A transfer issued while a kernel is still writing its source sends whatever
is there at the time. `TransferOptions::after` takes events recorded on the
application's stream, and the adapter waits for them before the adapter
touches the buffers.

Measured with `hux-bench --produce`, which enqueues repeated fills on a
stream the caller owns, records an event, writes the buffer to a peer, and
reads back what actually landed. 4 MiB, 400 fills ahead of the event, 20
rounds, A40 over RoCE:

| | arrived with what the producer wrote | per round |
| --- | --- | --- |
| `after=[ev]` | 20/20 | 2816.7 us |
| no ordering | 0/20 | 2194.4 us |

**The second row is the measurement.** A single arm reporting 20/20 would be
indistinguishable from a test whose producer happened to finish first, and
would prove nothing about the ordering. Failing every round without the
event is what establishes that the race is real and that the event is what
prevents it.

The 622 us between them is not overhead. It is the producer's remaining work,
which the ordered transfer waits for and the unordered one skips by sending
the wrong bytes.

This needs a card whose device memory the adapter can register. On an RTX
4090 the server reports `register failed` before any of this runs, for the
reason in [comparison.md](comparison.md): GPUDirect is withheld from the
consumer line, so the benchmark falls back to host memory where there is no
stream to order against.

## One transfer as several scattered segments

`--segments N` splits each transfer into N pieces at strided offsets and
sends them with `writev`, which is a different path from a contiguous
transfer: it pairs segments by index, submits them together, and has to
handle a partial submit. Nothing measured it before.

1 MiB as one piece against eight of 128 KiB, cross-machine, 6000 transfers
per point, three passes in alternating order:

| pass | 1 segment | 8 segments |
| --- | --- | --- |
| 1 | 34.60 | 44.29 |
| 2 | 85.99 | 85.77 |
| 3 | 85.42 | 85.75 |

The first pass is a cold start and it hit both arms. Excluding it, the two
are within 0.4% of each other: **scattering a transfer eight ways costs
nothing measurable here.**

The first attempt at this used 400 transfers per point rather than 6000, and
produced 61.35 / 86.16 / 41.55 for one segment against 85.99 / 86.09 / 86.14
for eight. By medians that is 8 segments winning by 1.4x, and it is not
real: 400 transfers of 1 MiB is 37 ms of work at this rate, so a single
scheduling hiccup is most of the measurement. The clue was in the numbers --
the contiguous arm's best result equalled the scattered arm's typical one,
which is what a disturbance looks like and not what a slower path looks
like. Fifteen times the work made the cold start land on the other arm
instead, which settles it.


## The two adapters on this host are not equal, and the reason is not the network

Both are 100 Gb/s Ethernet and both reach the same peer, but not at the same
rate. Three passes each in alternating order, 2000 transfers of 4 MiB,
each adapter selected by giving `--local` its own address:

| adapter | write | the three |
| --- | --- | --- |
| `mlx5_3` | 51.38 Gb/s | 51.45, 51.38, 51.08 |
| `mlx5_0` | 24.00 Gb/s | 24.02, 23.79, 24.00 |

A factor of 2.14, repeatable to within 1%. It is not the link: `ethtool`
reports 100000 Mb/s on both. It is the slot.

| adapter | PCIe | of a possible | NUMA | that allows | measured |
| --- | --- | --- | --- | --- | --- |
| `mlx5_0` | Gen3 x4 | x16 | node 0 | 31.5 Gb/s | 24.0 |
| `mlx5_3` | Gen3 x8 | x16 | node 1 | 63.0 Gb/s | 51.4 |

Both cards negotiated well below what they are capable of, and each lands at
about 80% of what its lanes allow. The network is not the constraint on
either one -- a 100 Gb/s port behind four Gen3 lanes cannot exceed 31.5.

Two things follow. A benchmark that does not say which address it advertised
has not said which adapter it measured, and a number from this host is
meaningless without it. And the adapters sit on different roots and different
NUMA nodes, so what one costs the other is a question worth asking rather
than assuming -- which is the ceiling check for NET-03 in
[remaining.md](remaining.md), not a reason to go and implement it.


## Do the two adapters add up? (the ceiling for NET-03)

Striping one transfer across both adapters is only worth building if two
flows, one per adapter, reach more together than the better adapter reaches
alone. If they share a bottleneck the sum is that bottleneck and the feature
buys nothing. Asked before writing any of it.

Two passes, 2000 transfers of 4 MiB per flow:

| | write |
| --- | --- |
| `mlx5_0` alone | 23.87 Gb/s |
| `mlx5_3` alone | 51.56 Gb/s |
| both at once | **75.38 Gb/s** (75.77, 75.00) |
| against the better one alone | **1.46x** |

They add. More precisely, each flow keeps its solo rate while the other runs:
23.90 with 51.87, and 23.78 with 51.22, against 23.87 and 51.56 alone. Within
1%, so at these rates the two adapters cost each other nothing -- which
follows from their sitting on different PCIe roots and different NUMA nodes,
but did not have to be true.

So the ceiling is 1.46x and the work is worth doing. Note what that number is
not: it is the ceiling for a perfect implementation on this host, with these
two adapters at this ratio. A striping policy that split a request evenly
would be limited by the slower adapter and reach about 48 Gb/s, which is less
than `mlx5_3` alone. Weighted by capacity is the only split that wins here,
and the weights are a measurement rather than a constant.

## Replaying a recorded workload

`--trace FILE` replays arrivals rather than issuing as fast as it can. One
record per line, `<at_us> <bytes> <r|w>`, times from the start of the replay.
`benchmarks/traces/bursty.trace` is a synthetic one -- quiet stretches broken
by clusters, 4 KiB to 1 MiB -- so the path has something to run against;
replace it with a recording of the workload you care about.

The number a replay reports is not throughput. Throughput says how fast this
end can go; a replay asks whether it kept up with work arriving on somebody
else's schedule, and the answer is how far behind each request went out.

317 records, 68.2 MiB over 0.453 s:

| behind schedule | median | p90 | p99 |
| --- | --- | --- | --- |
| the replayer alone | 0.2 us | 20.0 us | 27.5 us |
| replaying | 1.0 us | 20.9 us | 34.8 us |

**The first row is the point.** Waiting for a due time has a cost of its own,
and without measuring it every microsecond of that cost is charged to the
transport. Here the transport adds 0.8 us at the median and 7.3 us at p99 to
a floor the benchmark would have paid anyway.

An earlier version reported 45.4 us median and attributed all of it to the
transport. It was the sleep granularity in the waiting loop: sleeping up to
the deadline overshoots it by however long the sleep was. Spinning the last
50 us took the floor from 45 us to 0.2, and what remains is small enough that
the floor row is still needed to see it.

## Striping across machines, and what stopped it

Splitting a transfer needs two usable adapters at *each* end. Between the
host measured above and the peer -- a Cambricon MLU370-X8 machine -- there
is only one at the far end, and finding that out took longer than it should
have.

All four pairings, 200 transfers of 1 MiB each, the server waited for rather
than slept for, this host's adapter first:

| from | to | |
| --- | --- | --- |
| `mlx5_3` | the peer's first adapter | 49.97 Gb/s |
| `mlx5_0` | the peer's first adapter | 23.62 Gb/s |
| `mlx5_3` | the peer's second | connects, no transfer completes |
| `mlx5_0` | the peer's second | connects, no transfer completes |

The peer's second adapter reports `PORT_ACTIVE`, accepts a TCP connection,
completes the endpoint exchange and brings its queue pair to ready. It simply
cannot carry data: both hosts have two addresses on one subnet, and the
kernel route decides which interface a reply leaves by, so packets for the
second adapter go out the first and never come back.

**And that is a gap here, not only in the fabric.** A lane that connects and
cannot carry anything poisons every transfer through that peer: a request is
not complete until every lane's share is, so one silent lane hangs all of
them, with no timeout and nothing said. Striping on loopback worked because
both adapters are on one host and no route is consulted.

`connect` returning `kOk` used to mean the endpoints were exchanged and the
queue pair reached ready. That is too weak a claim to hand a caller a lane
on -- the same shape of mistake as a copy that returns before its bytes have
landed. It now means the queue pair has carried something: the handshake
ends by sending a few bytes and waiting for its own completion, which on a
reliable connection means the far end acknowledged them.

With that, the same three configurations behave as they should:

| | |
| --- | --- |
| the dead adapter alone | refused in 3.0 s, saying no adapter could carry a transfer |
| the working adapter alone | 51.26 Gb/s, 1.1 s |
| both, one of them dead | 51.40 Gb/s, 4.1 s |

The third row is the point. A lane that cannot carry anything now costs
three seconds once, at setup, instead of hanging every transfer for ever.
Both ends reach the same smaller set on their own: the client leaves out a
lane it cannot prove, and the server serves on the adapters that came up
rather than giving up because one did not.

An earlier version of this section blamed the fabric on the strength of a
test whose client started three seconds after the server, whether or not the
server was listening. That test reported `mlx5_0` to the peer's first
adapter as failing, which is the pairing measured at 24 Gb/s twice on either
side of it. Waiting for
the server to say it was ready changed three of the four answers.

## Why TIMELY delivered 2.97 Gb/s on a path that carries 91

The head-of-line run above measured it at 2.97 Gb/s against 91.27 for no
control at all, with a head-of-line penalty of 818x. That is not a tuning
problem, and the first explanation offered for it was wrong.

The guess was that its window -- rate times the smallest delay seen -- came
out smaller than a chunk, so every chunk took the escape for an operation
larger than the whole window and went out alone. Serialising 1 MiB chunks on
a path with a 15 us base delay would still give about 78 Gb/s, so that
cannot be it.

Asked of the controller directly, with `tests/manual/timely_probe.cpp`:

| operations | admitted | paced / 200000 | over budget | rate reached |
| --- | --- | --- | --- | --- |
| 1 MiB | 1.87 Gb/s | 199912 | 0 | 3.47 Gb/s |
| 128 KiB | 37.44 Gb/s | 98799 | 88507 | 100 Gb/s |
| 64 KiB | 56.57 Gb/s | 119518 | 45955 | 100 Gb/s |

The window refused nothing at all with 1 MiB operations. What refused them
was the pacing, on a rate that stopped at 3.47 Gb/s and never climbed.

**The increase was applied per completion rather than per round trip.** With
megabyte operations there are few completions, so the rate climbs slowly, so
there are fewer still -- a loop that never gets out of its own way. Small
operations complete often enough to escape it, which is why the same
controller reached line rate with 64 KiB.

Scaling the increase by how many round trips have passed, which is what the
paper specifies, with the same probe:

| operations | before | after |
| --- | --- | --- |
| 1 MiB, mixed with 16 KiB | 1.87 Gb/s | **41.45 Gb/s** |
| 1 MiB alone | 2.10 Gb/s | **54.53 Gb/s** |
| 128 KiB | 37.44 | 41.95 |
| 64 KiB | 56.57 | 59.31 |

The sizes that already worked are unchanged, which is the check that matters:
a fix that bought the large case by spending the small one would not be one.

## What a ready handoff costs, and why the earlier number was flattering

A write that lands sends the peer a notice saying which bytes arrived. It
travels on the control channel, which is a TCP socket with Nagle off, so a
notice per write is a packet per write.

This looked at first like a clear cost. The head-of-line page's "alone"
figure for a 16 KiB write moved from about 19 us to about 32 between two
runs of the same benchmark on the same pair of machines, and the only thing
that changed was the build. Four alternating rounds of old against new put
the old tree at 18.9, 19.0, 20.0, 20.1 us and the new one at 31.9, 33.0,
21.1, 23.1 -- with the 4 MiB figure unmoved at 2941 to 2946, so whatever it
was, it was a cost per request and not per byte.

It was not a regression. Varying both ends separately, three rounds each:

| server | client | read | write |
|---|---|---|---|
| old | old | 19.0 | 16.8 |
| old | new | 19.0 | 17.0 |
| new | old | 19.0 | 46.0 |
| new | new | 19.0 | 23.4 |

The read is 19.0 in all twelve runs; it carries no handoff. Changing the
client barely moves the write. The server is the whole difference -- and
what changed on the server is that it now runs a progress thread, so it
reads its control channel. The old one never did: the socket filled and the
handoffs stopped being delivered. **The 19 us baseline was fast because the
notices were being dropped.**

How much delivering them actually costs is not a number this fabric will
give. The write measurement is not repeatable to anything like the
precision needed: the same configuration that measured 23.4 us above
measured 34.5 us an hour later, and a set of twelve runs with a draining
receiver spread from 19.4 to 53.1 while the read alongside them stayed at
19.0 in every one. So: delivering the handoff raises the write's latency
and, more clearly, its variance. Any figure for how much is fiction.

Batching was tried on the strength of the packet-per-write argument --
queue a completion batch's notices and send them in one `sendmsg` instead
of one each. It could not be shown to help: old 38.4, one-at-a-time 34.5,
batched 34.2, against a within-configuration spread of 27 to 53. It also
made a control message wait for the next progress call, which is a trap for
any caller that sends one and then stops polling -- five IPC tests failed on
exactly that, because a peer that publishes a region and waits to be read
from has no progress loop to flush it. Reverted. Worth revisiting only with
a way to measure the control channel that the data path's variance does not
swamp.

## Incast: what a controller does when more is offered than the link carries

Every congestion-control measurement before this one ran on a link that was
full but not oversubscribed, which is the condition a controller is least
able to improve. Three machines writing to one changes that: they offer 162
Gb/s into a receiver that absorbs about 93.

Read the shares against what each sender can do alone, or they say nothing.
The three are not the same hardware -- 88.05, 50.93 and 23.37 Gb/s measured
one at a time against the same receiver, three passes each, spread under 0.5%
-- so a gap between them under load is mostly a gap in what they are.

Twelve runs, four controllers, overlap 0.99 or better in every one (the gate
that the windows really coincided; a first attempt without it reported 158
Gb/s over a 100 Gb/s adapter by adding up runs that did not overlap):

| cc | total Gb/s | fastest/slowest | median us |
|---|---|---|---|
| off | 93.54 | 2.26x | 1022.8 |
| fixed 1 MiB | 93.63 | 2.27x | 1046.7 |
| timely (paper) | 93.11 | 1.99x | 1073.2 |
| timely, 1000x increase | 93.33 | 1.84x | 1086.7 |

The ratio between fastest and slowest is the wrong way to read this. Most of
it is capability: the fastest sender is getting 47.9% of what it can do
alone and the slowest is getting 80.0%, which is the opposite of one taking
the link from the other.

Against max-min fairness -- the share each would get if the capacity were
divided as evenly as their ceilings allow, which here is 23.37, 35.07 and
35.07 -- there is a real effect, and it is modest:

| sender | alone | no control | of fair share | timely:1000 | of fair share |
|---|---|---|---|---|---|
| hygon1 | 88.05 | 42.2 | 120% | 40.2 | 115% |
| 232 | 50.93 | 32.4 | 92% | 31.2 | 89% |
| 233 | 23.37 | 18.7 | 80% | 21.9 | 94% |

So the controller takes the slowest sender from 80% of its fair share to
94%, and the fastest from 120% down to 115%, for 0.2% of the aggregate and
6% of the median latency. That is what a window is worth under overload
here: real, small, and nothing like what the raw spread suggests.

## Host memory behind an IOMMU wants huge pages

Between two GH200s over 400 Gb/s InfiniBand, 4 MiB writes from host memory
ran at 92 Gb/s — a quarter of what the same port carried from GPU memory.
Grace translates the adapter's accesses through its SMMU, and a buffer on
ordinary 4 KiB pages is a translation per 4 KiB. Only the pages changed
between these runs (alternating, two passes each, both ends):

| 4 MiB write | 4 KiB pages | 2 MiB pages |
| --- | --- | --- |
| one queue pair | 362–373 us | 105 us (320 Gb/s) |
| four queue pairs | 300–313 us | 98–104 us |
| read, four queue pairs | 271–282 us | 91 us (367 Gb/s) |

More queue pairs do not buy this back: past two they were slower, which is
the translation path saturating, not the link. The other transport measured
on the same machines gained the same factor from the same change, so this
is a property of the buffer, not of a library.

`hux::alloc_host()` (Python: `hux.alloc_host(n)`) returns memory on 2 MiB
pages, faulted in before anything registers it, and reports how much the
kernel actually granted — with transparent huge pages set to `never` it is
ordinary memory that still works. A caller that owns its staging buffers
should take them from there. hux-bench measures with it under
`--hugepages on`.
