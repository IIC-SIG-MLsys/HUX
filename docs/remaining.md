# What is left, and what each piece needs

The roadmap numbers 23 required tasks, plus six conditional ENV items that
are not counted here. Four of the 23 are outstanding and listed below --
CC-02, REL-01, TST-02 and SCH-02 -- so nineteen are done.

An earlier version of this page said nineteen of twenty-six, which was wrong
in both halves by the same three -- the denominator was never counted from
the roadmap and the numerator was carried along with it. Counted from the
list: API-01, BACK-01, CC-01, COR-01, CTL-01, DEV-01, FAIL-01, MEM-01,
MEM-02, MIG-01, NET-01, NET-02, NET-03, NTF-01, PY-01, SCH-01,
TOP-01, TST-01, BENCH-01.

What is left is below, with what each one is actually blocked on, because
most of them are not blocked on writing code.

## Blocked on hardware or machine time

**NET-03, several NICs carrying one transfer. Done.** An earlier version of
this entry called it blocked, on the grounds that only `mlx5_0` was active
here. That was read off the first
three adapters. `mlx5_1` and `mlx5_2` are indeed down; `mlx5_3` is up, and
so this host has two. The peer used for this -- a Cambricon MLU370-X8
machine -- has two as well.

Checked rather than assumed, since the claim had already been wrong once: a
transfer runs over each of this host's adapters independently, chosen by the
address the provider is told to advertise. Giving `--local` the address on
`mlx5_0` sends over `mlx5_0`, giving it the one on `mlx5_3` sends over
`mlx5_3`, and both reach the same peer.

The ceiling was measured before writing any of it: two flows, one per
adapter, reach 75.38 Gb/s against 51.56 for the better adapter alone, a
**1.46x** headroom, and each flow keeps its solo rate while the other runs.
See [tuning.md](tuning.md). A split that ignored the 2.14x between them
would be held to the slower adapter and finish below `mlx5_3` on its own, so
the weights are part of the feature rather than a refinement of it.

**Implemented, and measured on loopback at 1.33x** -- 48.60 Gb/s over one
adapter against 64.54 over two, three alternating passes. Across machines it has only one
adapter to work with, because the peer's second cannot carry data, and it
degrades to that one and runs at full single-adapter speed: 51.40 Gb/s
against 51.26 for that adapter alone, the difference being three seconds
spent proving the other one at setup.

That degradation had to be built. A lane that connects and carries nothing
used to hang every transfer through the peer, since a request is not
complete until every lane's share is. `connect` now ends by sending a few
bytes and waiting for its own completion, so returning ok means the queue
pair has carried something rather than that the endpoints were exchanged.
See [tuning.md](tuning.md).

What is not demonstrated is striping between two machines, and that is the
fabric rather than the feature: both hosts here put two adapters on one
subnet without `arp_ignore` or `arp_announce`, so each machine's second
adapter is unreachable -- replies leave by the first interface with the
wrong source and the queue pair never matches them. UCCL meets the same wall
on the same pair and hangs instead of degrading. Fixing it means changing
network configuration on shared machines, which is not this library's to
change.

**Incast, part of TST-02.** Not blocked on machines: four hosts sit on this
RoCE fabric, not the two the earlier version of this entry assumed. Three
senders at one receiver is the only way to overload it -- depth cannot, it
only queues -- so this is also the first condition under which the
congestion controllers are asked what they exist for.

Measured once as a baseline, with the senders lined up on a wall clock and
run for a fixed duration rather than a fixed count: 93.77 Gb/s arriving,
shared 2.24x unevenly, the three overlapping 99% of the time. Getting that
right took four attempts -- the first reported 166 Gb/s over a 100 Gb/s
adapter by adding rates measured at different moments. The comparison across
congestion-control settings is what remains.

**REL-01, the 24-hour verification.** The harness exists --
`tests/manual/soak.cpp`, built as `hux_soak` -- and crosses every dimension
at once rather than one at a time: several peers, several threads, both
directions, sizes from 4 KiB to 4 MiB interleaved, host or device memory,
and a registration dropped and remade every few hundred rounds.

Every round verifies its bytes, because a transport defect that is not a
crash is a wrong byte and a soak that only counted completions would run for
a day and report success while moving rubbish. Reads come from a half of the
peer's memory that carries that peer's own pattern and is never written;
writes go into a slice of the other half belonging to that thread alone, so
one thread cannot invent a failure for another. It also watches the engine's
own counters, and stops if a transfer is ever staged rather than done in
place.

Verified two ways: a short run moves 33 GiB over 33,000 rounds with nothing
copied, and a deliberately wrong pattern on the server is caught after two
rounds. A soak that cannot fail is not a soak.

What is left is the day itself, and it does not fit: by local convention a
run that loads a shared fabric belongs between 00:00 and 06:00, and 24 hours
does not. `--pause-ms` exists so the soak can run at a declared fraction of
the link rather than as fast as it goes, which is the thing to settle before
booking a day.

## Not blocked, just not done

**FAIL-01, peer restart and reconnect.** Departure is handled on both
transports. A provider reports it through `ProviderConnection::alive()` --
the IPC path when its socket closes, the RDMA path when its control channel
reads end of file, which it used to treat the same as having nothing to read.
Every progress turn retires peers whose connection has gone, fails what was
in flight with `kPeerDisconnected`, and marks writes as possibly having
reached the target, because after a disconnect there is no way to find out.
Measured on real RDMA: a client notices a departed peer 0.2 s after it exits,
which is the polling interval.

Restart is covered too, and it needed a fix rather than a test. A peer that
comes back is a new engine, and region ids are handed out per engine from
one, so its predecessor's descriptors went on looking current. They now carry
the identity of the engine that exported them and an import into anything
else is refused with `kStaleGeneration`. Before that the descriptor imported
cleanly and the transfer failed several calls later with `kInvalidArgument`,
which describes the argument rather than the peer.

Reconnection itself is what remains, and it is not worth having as a separate
mechanism: `remove_peer` plus `add_peer` rebuilds the path, and the only
thing a reconnect would add is keeping one `PeerId` across it. That is worth
less than it sounds, because the peer's regions have to be re-exported and
re-imported anyway -- the check above is precisely what stops the old
descriptors being reused -- so the application is already doing the work a
preserved id was meant to save.

**CC-02, an adaptive controller that meets a target.** Compared against off
and a fixed window under four concurrent flows on a saturated link, and they
cannot be told apart: see [tuning.md](tuning.md). Fairness is 1.00x with
control off, since RC transport provides it, so there is nothing there to
improve; latency varies more between passes of one configuration than
between configurations.

What is not covered is overload rather than saturation -- more offered load
than the link can carry, which is where a controller earns its place. The
benchmark can now hold many requests in flight, so this is reachable on this
pair; incast from more machines is not.

**TST-02, the integration matrix.** Most dimensions are covered
individually: several queue pairs, both directions, host and device memory,
registration reuse, notification back-pressure, failure paths, six threads
issuing mixed sizes at once, and several peers served concurrently with the
bytes checked per peer. Crossing them rather than taking them one at a time
is what `hux_soak` does, so that half is no longer missing -- only the run
length is, and that is REL-01's problem above.

What remains is the part that needs hardware this pair does not have:
incast, and one request striped across several NICs.

**BENCH-01. Done.** `hux-bench` measures latency and bandwidth at fixed sizes
across two machines, with a chosen number of requests in flight, a stream of
interleaved sizes measured against each size run alone, the processor time a
transfer costs, and several peers at once.

`--peers N` talks to N servers on consecutive ports from one engine. Each
server stamps its memory with a byte of its own and the client checks what
came back, which is the part worth having: a read landing on the wrong
peer's region succeeds and returns the right number of bytes, so only the
contents say anything is wrong. That is the defect found on 2026-09-21, and
this would have caught it.

`--produce` puts work on the caller's own stream ahead of a transfer and
checks what arrived, running the arm without the ordering as a control. It
fails every round, which is what establishes that the ordering does
something: see [tuning.md](tuning.md).

`--segments N` sends one transfer as several scattered pieces through the
vector path, which nothing had exercised. Eight ways costs nothing
measurable: see [tuning.md](tuning.md).

`--trace FILE` replays a recorded arrival pattern and reports how far behind
schedule each request went out, against a floor measured by running the same
waiting loop with nothing submitted -- because waiting for a due time costs
something, and without that row the benchmark's own granularity is charged to
the transport. The ablations are in [tuning.md](tuning.md): queue pairs,
chunk size, congestion control, requests in flight, segments, adapters.

**BENCH-01 is done.**

**PY-01. Done.** Complete, stream and event adapters included. A build with
a device backend exposes `import_stream`, `record_event` and
`stream_wait_event`, and every submission takes `after=[...]`, so a transfer
can be ordered against the caller's own GPU work rather than a device-wide
synchronise. The adapter takes a native handle as an integer, which is how
frameworks expose one, so nothing here depends on torch.

Verified against a real CUDA stream created outside the library, on both
kinds of build: with a backend the adapters work and gate a transfer, and
without one they raise instead of dereferencing the backend that is not
there.

## Recommended for removal rather than implementation

**SCH-02's automatic profile.** The sweep in [tuning.md](tuning.md) answers
this in the negative for this fabric: one setting moves anything, its useful
range is a single step, and that step is now the default. A profiler would be
a mechanism for rediscovering a constant. Aging and load-aware queue-pair
selection remain unimplemented, and would only start to matter under the
contention that has not been measured yet — so they should be decided after
incast, not before.

## Where the numbers came from

Seventeen done, seven partial, two untouched. Weighted by effort rather than
counted, roughly three quarters. The remaining quarter is not evenly
distributed: two of the largest items are untouched, and three of the
partial ones are waiting on machines rather than on work.
