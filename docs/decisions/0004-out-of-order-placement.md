# Out-of-order placement, through the adapter's own commands

Status: decided, 2026-09-23.

## Question

A 1 MiB write from an adapter on PCIe x8 into one on x4 lost a packet about
one write in three, and every loss cost a go-back-N retransmission of the
rest of the message and a DCQCN rate cut: a p99 of 1.5 ms against a median
of 363 us. UCX on the same path lost nothing, because it brings its queue
pairs up with out-of-order data placement, where each packet carries its own
address and lands whenever it arrives. Forced back to the standard ordering,
UCX lost the same packets and had the same tail. The question was how HUX
gets the same, on adapters verbs does not offer it for.

## What it does

Where both ends' adapters can place RDMA reads and writes out of order --
mlx5, RoCE v2 over IPv4, not bonded -- a connection's queue pairs are
created and taken to INIT through verbs, then to RTR and RTS by the
adapter's own commands (DEVX), with the ordering field set and every other
field written as the kernel writes it for `ibv_modify_qp`. The context is
read back afterwards and the connection refused unless the adapter holds
what was asked. Each end offers the feature in a word exchanged after the
endpoints (wire minor 1), and a connection uses it only when both offer it.
`describe()` says whether this end offers it, why not when it does not, and
how many connections have it.

## Alternatives

**rdma-core's `MLX5DV_QP_CREATE_OOO_DP`.** The supported way, and not
available: it asks for out-of-order sends and receives as well, and the
kernel refuses it without `dp_ordering_ooo_all_rc`, which ConnectX-5 does not
have. These adapters have out-of-order reads and writes only, which is also
what UCX uses on them.

**Queue pairs created through DEVX.** What UCX does. It would mean posting
work requests and polling completions without verbs -- a second data path to
keep correct, for one field in one command.

**Chunking at the MTU.** Removes the loss -- measured, nothing out of
sequence -- but posts 1024 work requests per MiB and halves the throughput.

**Pacing the queue pair in hardware.** The rate that avoids loss is the
receiver's, which the sender does not know and which changes with every
other flow into that receiver.

## What that forced

**Fields the kernel takes from the system.** Brought up through verbs, a
queue pair's hop limit is replaced by the route's when the kernel resolves
the destination, whatever the caller asked for, and MLNX_OFED can override
the traffic class per port and per destination. The hop limit is taken the
same way here. The traffic class override is not reproduced: where it is
set, the feature is not offered, rather than putting these queue pairs in a
different class from every other one on the adapter.

**The completion queue is left to verbs.** `mlx5dv_init_obj` on
a completion queue marks it as owned by the caller, and verbs then stops
removing a destroyed queue pair's completions from it -- the next poll finds
one for a queue pair that no longer exists and fails. The numbers the
commands need are read back from the queue pair's own context instead.

**A failure is not retried through verbs.** The peer is bringing up its end
out of order; a connection whose two ends disagree is one nothing here has
been run on. The connection is refused with a device error, and
`RdmaConfig::out_of_order = false` is the way out.

## What an application has to know

The bytes of one message can land in any order, so nothing may watch the
last byte of a buffer to learn that the rest is there: arrival is what the
completion, or the ready handoff, says. Completions are unchanged. They
arrive in the order work was posted, so a signalled completion still covers
the unsignalled ones before it -- the assumption UCX makes too, retiring
every operation up to a completion's work-request counter, with this
ordering on by default.

## What was measured

See [comparison.md](../comparison.md#with-out-of-order-placement). On the
x8 -> x4 path the p99 went from 1480 to 381 us with no packet lost in any
run, and against UCX's put from 4.3x to 6% apart; on a rate-matched path,
where nothing is lost either way, the per-packet headers cost about 3 us in
370. The command encoding is tested without hardware against a context read
back from a ConnectX-5.

That every other field is written as the kernel writes it was checked on
the adapter, over the whole queue pair context. Brought up by these
commands with the ordering left standard, a queue pair's context matched
one brought up through verbs in every word but those that differ between
any two queue pairs -- the remote number, the UDP port derived from it, the
doorbell address and one reserved word. With the ordering set, the only
further differences were the ordering bit and two reserved bits the adapter
changes along with it.
