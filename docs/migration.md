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

**Descriptors carry a generation and an origin.** A peer that deregisters
tells you, and the imported region stops accepting submissions. A descriptor
also names the engine that exported it, and importing it into any other peer
is refused with `kStaleGeneration` -- including the peer that replaced it
after a restart, which is a different engine however identical its address.
Region ids are handed out per engine from one, so without that check a
predecessor's descriptors go on looking current. Holding a descriptor is not
permission to use it forever, or on whichever peer is to hand.

## What is not carried over

`ConnBuffer`, the pipeline entry points, and the collective experiments. The
collective paths had known correctness problems and no test that would have
caught them; rebuilding them on this API is possible but has not been done,
and they are not part of what this library claims to do.

HMC remains available for callers that need it.

## Worked examples

Each pair does the same thing. The HMC side is the shape the old API forced;
the HUX side is what to write instead.

### A one-off write

HMC stages through the library's buffer, so the application's data is copied
into it first and the transfer names offsets into that buffer:

```cpp
auto buf = std::make_shared<ConnBuffer>(device_id, 64 << 20);
Communicator comm(buf);
comm.connectTo(peer_id, self_id, "10.0.0.2", 18515, ctrl_link, ConnType::RDMA);

buf->writeFromGpu(my_data, len);            /* copy in */
comm.put("10.0.0.2", 18515, 0, 0, len);     /* offsets into buf */
```

HUX registers the memory the application already has, once, and transfers
name views into it:

```cpp
EngineConfig cfg;
std::unique_ptr<Engine> engine;
make_engine(cfg, nullptr, provider, &engine);

MemoryRegionPtr region;
engine->register_memory(my_data, len, AccessFlags::kLocalRead, &region);

PeerPtr peer;
engine->add_peer(peer_metadata, &peer);     /* see below */
RemoteRegionPtr remote;
peer->import_region(peer_descriptor, &remote);

RegionView src, dst;
region->view(0, len, &src);
remote->view(0, len, &dst);

RequestPtr req;
engine->write(peer.get(), src, dst, {}, &req);
req->wait(5000);
```

Longer, and that is the point: registration, connection and transfer are
three separate lifetimes in HUX, where HMC hid all three behind one call and
paid a copy for it. Register once at startup and the per-transfer part is the
last five lines.

`peer_metadata` and `peer_descriptor` are opaque blobs from
`engine->local_metadata()` and `region->export_descriptor()` on the other
side. Getting them across is the application's business -- a socket, a file,
whatever the job already has. HMC took an IP and a port on every call
instead, which is why it could not describe a peer reached over shared
memory.

### Many writes at once

HMC issues them one at a time and collects work-request ids:

```cpp
std::vector<uint64_t> ids;
for (auto const& c : chunks) {
  uint64_t id;
  comm.putNB("10.0.0.2", 18515, c.local_off, c.remote_off, c.size, &id);
  ids.push_back(id);
}
comm.wait(ids);
```

HUX takes the whole list in one call, so the queue is filled once rather than
per chunk:

```cpp
std::vector<RegionView> src, dst;
for (auto const& c : chunks) {
  RegionView a, b;
  region->view(c.local_off, c.size, &a);
  remote->view(c.remote_off, c.size, &b);
  src.push_back(a);
  dst.push_back(b);
}
RequestPtr req;
engine->writev(peer.get(), src, dst, {}, &req);
req->wait(5000);
```

One `Request` covers the batch. It is not atomic: on failure,
`req->error().may_have_modified_target` says whether the target may already
hold some of it, and nothing is replayed for you.

`putPipeline` has no equivalent call because it is not a call any more.
Chunking and the number of transfers in flight are `EngineConfig::chunk_bytes`
and the engine's own business; a single `write` of a large region is already
pipelined.

### Waiting for a specific stage

HMC's `wait` returns when the local completion arrives, which for a write
means the NIC has read the source -- not that the target holds the data:

```cpp
comm.wait(wr_id);   /* source reusable; the target may not be ready */
```

HUX makes the difference visible, so code that needs the stronger guarantee
asks for it and code that only needs the buffer back does not pay for it:

```cpp
req->wait(5000);
if (req->reached(Stage::kSourceReusable)) { /* safe to refill my buffer */ }
if (req->reached(Stage::kTargetReady))     { /* the peer can read it */ }
```

On the RDMA path a write reaching the target is reported by the peer, not
inferred locally, so `kTargetReady` arrives later than `kSourceReusable` and
sometimes much later. Ask for the one the algorithm needs.

### Telling the peer something arrived

HMC pairs a tag send with a tag receive over a separate control link:

```cpp
comm.ctrlSend(peer, tag);          /* sender */
comm.ctrlRecv(peer, &tag);         /* receiver */
```

HUX carries a payload and orders it after the data:

```cpp
engine->notify(peer.get(), payload, &req);          /* sender */
engine->poll_notifications(8, &notes);              /* receiver */
```

The receiver can also skip the handshake: `poll_ready_events` reports regions
a peer has written, so a receiver that only needs to know *what* changed does
not need the sender to tell it.

## Compatibility

HUX does not speak HMC's wire protocol. The two cannot be mixed in one job,
and there is no shim -- the object model differs by exactly the things that
would have to appear on the wire, so a translation layer would have to
reintroduce the staging buffer this library exists to remove. Migrate both
ends together.

Between HUX builds, four version numbers are negotiated, each covering one
layer, each with the same rule: **a different major is refused outright; a
newer minor is accepted**, because minor changes only append fields an older
reader stops before. Refusing rather than parsing as far as it goes is
deliberate. A major change moves fields, so a best-effort read takes one
field out of a neighbour's bytes and then acts on it.

| Layer | Constant | Checked in | On mismatch |
| --- | --- | --- | --- |
| Engine metadata | `Engine::kMetadataMagic` / `kMetadataMajor` | `add_peer` | `kUnsupported` before dialling |
| RDMA handshake | `kWireMajor` | connection setup | `kUnsupported`; no queue pair is created |
| IPC handshake | `kIpcWireMajor` | socket handshake | `kUnsupported`; the socket is closed |
| Region descriptor | `kDescriptorMajor` | `import_region` | `kUnsupported`; nothing is imported |

The descriptor is at major 2. Major 1 did not name the exporting engine, so
an importer could not tell one peer's region 1 from another's; a peer built
against it is refused rather than read.

The engine metadata blob also begins with a magic number, so `add_peer` can
tell an engine's metadata from a single provider's dialling blob by looking
rather than by attempting a parse and keeping it if it fits. Length-prefixed
structures can parse by coincidence, and the two layouts are far enough apart
that guessing wrong dials the wrong transport with the wrong bytes. The
provider blob remains accepted, unnamed, for tools that point a client at an
address by hand.

None of these numbers has changed since the first build that had it, so
nothing in the field has yet had to refuse a peer over version. That is a
statement about age, not about the checks: they are covered by
`tests/contract/test_metadata_envelope.cpp` and
`tests/contract/test_wire_version.cpp`, in both directions.

What is *not* promised across builds: `EngineStats` field meanings, the text
of `describe()`, and the contents of the provider blob. They are diagnostics
and internal dialling information, they are not version-tagged, and nothing
should parse them.
