# UCX provider

Built with `-DHUX_ENABLE_UCX=ON`, which requires the UCX development files.

## Why it needs its own completion handling

A UCX put reports completion when the source buffer is free to reuse, which is
not when the data has reached the peer. Only a flush settles that. Treating
the first as the second would let a caller hand data to a consumer the peer has
not received, so this provider does not report a write complete until a flush
of its endpoint has finished -- one flush per endpoint per poll, covering every
write on it that came before -- and declares `needs_explicit_flush` in its
capabilities.

Every UCX call for the worker is made under one lock. The library does not
serialize them, a worker driven from two threads corrupts quietly rather than
failing, and two do drive it: the caller submits while the progress thread
polls.

A request is reset before it goes back to UCX. UCX initializes a request's
memory only the first time it allocates it, so one freed as it was carried its
finished state into the next operation that got it: a read, or a flush, was
reported done before it had begun.

These three were fixed together on 2026-09-23. Before that the provider
reported finished puts without flushing them, drove the worker from two
threads, and freed requests as they were. `tests/manual/ucx_loopback.cpp`
checks each transfer the moment it reports done; over RC on one adapter the
previous provider had 292 of 2000 reported before their bytes had landed, all
of them reads, and the current one none in 4000.

The remote key a transfer uses is looked up among this provider's own
registrations, so both engines of a transfer have to share one
`UcxProvider`. A peer in another process cannot be reached until the packed
key travels in the region descriptor.

## Verified

`tests/manual/ucx_loopback.cpp`, one engine as its own peer with the progress
thread running, 64 B to 4 MiB both ways, every transfer checked, 2026-09-23:
2000 rounds and nothing wrong under `UCX_TLS=self,sm`, and the same over RC on
one adapter (`UCX_TLS=rc,ud,self`).

Two engines in one process, 1 MiB, both directions, 2026-09-18:

| `UCX_TLS` | read | write |
| --- | --- | --- |
| `self,sm` | verified, 0 bytes copied | verified, 0 bytes copied |
| default | `proto get/zcopy, not implemented` | never completes |

The provider works; the default transport selection on this host does not.
With the default configuration UCX picks a transport whose zero-copy RMA
protocol is unavailable in this build, and aborts inside the library rather
than returning an error the provider could report. Setting `UCX_TLS` to a
transport that supports it is the workaround, and choosing one automatically
would need probing the protocols UCX actually selected, which it does not
expose before the first operation.

Not yet done: control messages over UCX active messages. `send_control`
returns `kUnsupported` rather than dropping them silently, and there is no
other channel to fall back to. To a peer reached over UCX a notification is
refused, a ready handoff is counted in `ready_handoffs_failed`, and a notice
that a region has gone in `region_invalidates_failed`.

## Compared with the native RDMA provider

Both transfer application memory in place and report zero copies. The RDMA
provider handles multiple queue pairs, congestion control and per-queue
accounting; this one does not, and is here because parts of the support matrix
reach hardware the native path does not.
