# What is left, and what each piece needs

The roadmap numbers 23 required tasks, plus six conditional ENV items that
are not counted here. Six of the 23 are outstanding and listed below, so
seventeen are done.

An earlier version of this page said nineteen of twenty-six, which was wrong
in both halves by the same three -- the denominator was never counted from
the roadmap and the numerator was carried along with it. Counted from the
list: API-01, BACK-01, CC-01, COR-01, CTL-01, DEV-01, FAIL-01, MEM-01,
MEM-02, MIG-01, NET-01, NET-02, NTF-01, PY-01, SCH-01, TOP-01,
TST-01.

What is left is below, with what each one is actually blocked on, because
most of them are not blocked on writing code.

## Blocked on hardware or machine time

**NET-03, several NICs carrying one transfer.** Not started. Selection by
proximity works and is measured; striping one request across NICs is not
implemented. It needs a host with two NICs that are both up — on the machine
used here `mlx5_1` and `mlx5_2` are both DOWN and only `mlx5_0` is active, so
this cannot be written honestly, let alone measured.

**Incast, part of TST-02.** Needs three or more machines pushing at one
target at the same time, and by local convention that kind of run happens
between 00:00 and 06:00 so it does not disturb anyone.

**REL-01, the 24-hour verification.** Literally a day of machine time, and
probably more than one attempt: the longest run so far found a real defect
after 48 minutes.

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
bytes checked per peer. What is missing is the part that needs hardware this
pair does not have -- incast, and one request striped across several NICs --
and the long-running crossings of those dimensions rather than one at a
time.

**BENCH-01.** `hux-bench` measures latency and bandwidth at fixed sizes
across two machines, with a chosen number of requests in flight, a stream of
interleaved sizes measured against each size run alone, the processor time a
transfer costs, and several peers at once.

`--peers N` talks to N servers on consecutive ports from one engine. Each
server stamps its memory with a byte of its own and the client checks what
came back, which is the part worth having: a read landing on the wrong
peer's region succeeds and returns the right number of bytes, so only the
contents say anything is wrong. That is the defect found on 2026-09-21, and
this would have caught it.

Still missing: trace replay, a producing and consuming kernel on the
critical path, and the ablations.

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
