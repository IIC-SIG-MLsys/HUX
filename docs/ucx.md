# UCX provider

Built with `-DHUX_ENABLE_UCX=ON`, which requires the UCX development files.

## Why it needs its own completion handling

A UCX put reports completion when the source buffer is free to reuse, which is
not when the data has reached the peer. Only a flush settles that. Treating
the first as the second would let a caller hand data to a consumer the peer has
not received, so this provider does not report a transfer complete until the
flush says so, and declares `needs_explicit_flush` in its capabilities.

Every UCX call for one worker goes through one thread. The library does not
serialize them, and a worker driven from two threads corrupts quietly rather
than failing.

## Verified

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
returns `kUnsupported` rather than dropping them silently, and the engine uses
its own channel.

## Compared with the native RDMA provider

Both transfer application memory in place and report zero copies. The RDMA
provider handles multiple queue pairs, congestion control and per-queue
accounting; this one does not, and is here because parts of the support matrix
reach hardware the native path does not.
