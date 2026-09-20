# What is left, and what each piece needs

Against the 26 required tasks in the roadmap (the six ENV items are
conditional and not counted). Seventeen are done. This is the rest, with what
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

**FAIL-01, peer restart and reconnect.** A peer that exits is now detected
and refused on the IPC path, and `ProviderConnection::alive()` exists for a
provider to report it. Two pieces are missing: the RDMA provider does not
distinguish a closed control channel from an empty one (`recv` returning 0
and -1 are both treated as "nothing to read"), and the engine does not yet
act on `alive()` — so in-flight requests to a departed peer are not failed
and `Peer::connected()` stays true. Reconnection itself is a separate
question and may not be worth having: `remove_peer` plus `add_peer` already
rebuilds the path, and the only thing reconnection adds is keeping the same
PeerId.

**CC-02, an adaptive controller that meets a target.** TIMELY is implemented
and runs over a real fabric, where its window closes on measured delay. What
is missing is the comparison that makes it a result: the same workload
against off and a fixed window, with recovery behaviour, on a path where
congestion actually occurs. An idle two-machine fabric cannot produce that.

**TST-02, the integration matrix.** Many of its cells are covered
individually — several queue pairs, both directions, device and host memory,
registration reuse, notification back-pressure, failure paths. What does not
exist is the matrix: those dimensions crossed, with several peers and several
threads at once.

**BENCH-01.** `hux-bench` measures latency and bandwidth at fixed sizes
across two machines. Trace replay, mixed long and short requests, several
peers, a producing and consuming kernel, and the ablations are not there.

**MIG-01.** `docs/migration.md` covers the mapping from the old interfaces.
Worked examples and a compatibility statement are not written.

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
