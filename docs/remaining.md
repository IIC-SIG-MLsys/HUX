# What is left, and what each piece needs

Against the 26 required tasks in the roadmap (the six ENV items are
conditional and not counted). Eighteen are done. This is the rest, with what
each one is actually blocked on, because most of them are not blocked on
writing code.

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
transports now. A provider reports it through `ProviderConnection::alive()`
-- the IPC path when its socket closes, the RDMA path when its control
channel reads end of file, which it used to treat the same as having nothing
to read. Every progress turn retires peers whose connection has gone, fails
what was in flight with `kPeerDisconnected`, and marks writes as possibly
having reached the target, because after a disconnect there is no way to find
out. Measured on real RDMA: a client notices a departed peer 0.2 s after it
exits, which is the polling interval.

Reconnection itself is what remains, and it may not be worth having:
`remove_peer` plus `add_peer` already rebuilds the path, and the only thing
reconnection adds is keeping the same PeerId across it.

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
across two machines. Trace replay, mixed long and short requests, several
peers, a producing and consuming kernel, and the ablations are not there.

**MIG-01. Done.** [migration.md](migration.md) has the object and call
mapping, four worked examples against the real HMC signatures -- a one-off
write, a batch, waiting for a particular stage, and telling the peer -- and a
compatibility statement covering the four version numbers and what each
refuses. The HUX half of every example compiles against the headers.

**PY-01.** Complete except for stream and event adapters, which need a device
backend exposed to Python — nothing in the bindings currently touches a GPU.

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
