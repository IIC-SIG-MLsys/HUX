# Moving from HMC

HUX is not a renamed HMC. The object model changed because the old one made
the common case expensive: every transfer went through a buffer the library
owned, so application data was copied in and out of it.

A working example is in [`apps/transfer_example.cpp`](../apps/transfer_example.cpp).

## Objects

| HMC | HUX | Note |
| --- | --- | --- |
| `ConnBuffer` | — | No library-owned buffer. Register the memory you already have. |
| `Communicator` | `Engine` | Owns registrations, peers and progress. Several may coexist in one process. |
| IP and port per call | `Peer` | A stable handle. Connection identity no longer travels with every request. |
| `Memory` / `MemoryBase` | `DeviceBackend` | Device capabilities only; allocation is the application's business. |
| `wr_id` | `Request` | An object with a state, an error and a lifetime, not an integer. |
| — | `MemoryRegion` / `RemoteRegion` | Registered memory and a peer's exported view of it. |

## Calls

| HMC | HUX |
| --- | --- |
| `ConnBuffer(dev, size)` then `writeFromGpu` | `engine->register_memory(ptr, len, access, &region)` |
| `put(ip, port, local_off, remote_off, size)` | `engine->write(peer, local_view, remote_view, {}, &req)` |
| `get(ip, port, ...)` | `engine->read(peer, local_view, remote_view, {}, &req)` |
| `putNB(...)` returning `wr_id` | the same `write`, which is asynchronous already |
| `wait(wr_id)` | `req->wait(timeout_ms)` |
| `putPipeline(..., chunk, inflight)` | `EngineConfig::chunk_bytes`, handled by the engine |
| `sendDataTo` / `recvDataFrom` | `write` / `read` with a descriptor exchanged beforehand |
| `ctrlSend` / `ctrlRecv` | `engine->notify()` and `poll_notifications()` |

## What behaves differently

These are the changes that matter, and none of them is a rename.

**Memory is yours.** `register_memory` takes any pointer the application
already holds, and transfers name views into it. Register a pool once rather
than a buffer per transfer: registration is expensive and reuse is automatic
for ranges already covered.

**Nothing is copied on the way.** HMC staged through `ConnBuffer`, so every
transfer paid a device copy at each end. Here the NIC reads and writes your
memory directly, and `EngineStats::payload_bytes_copied` reports zero to prove
it. A path that cannot do this — Moore Threads, whose device memory cannot be
registered — says so through `DeviceCaps` instead of copying silently.

**A timeout is not a cancellation.** `wait(timeout)` returning `kTimeout`
means this wait ended, not that the request stopped. The request is still in
flight, its memory is still in use, and DMA may still be running. To stop it,
call `cancel()` and wait for `cancelled_safe`.

**`kWouldBlock` is not a failure.** The request was never accepted and had no
network side effect. Retry it; do not treat it as an error.

**Completion is not one event.** `source_reusable`, `transfer_complete` and
`target_ready` are separate, and a write is not complete when its local
completion arrives. Ask `req->reached(stage)` for the one you need.

**Failures say what they touched.** `error().may_have_modified_target` tells
you whether the remote side may already have been written. Batches are not
atomic, and nothing is replayed automatically.

**Descriptors carry a generation.** A peer that deregisters tells you, and the
imported region stops accepting submissions. Holding a descriptor is not
permission to use it forever.

## What is not carried over

`ConnBuffer`, the pipeline entry points, and the collective experiments. The
collective paths had known correctness problems and no test that would have
caught them; rebuilding them on this API is possible but has not been done,
and they are not part of what this library claims to do.

HMC remains available for callers that need it.
