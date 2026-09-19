# The same-host path maps, and says what that costs

Status: decided, 2026-09-19.
Measured on one host, two processes, one RTX 4090, CUDA IPC.

## Question

The roadmap asks for one set of interfaces over three cases: same process,
same host across processes, and across machines. The first two were done --
direct addressing and the network -- and the middle one fell back to RDMA. A
fallback that works is easy to leave alone, so the question is what a real
same-host path has to do that the network path does not.

## What it does

The peer's allocation is named, mapped into this process, and the bytes are
copied across the mapping. That is one copy through the GPU's own copy engine
instead of two trips across PCIe through the NIC, and on a host whose cards
cannot do GPUDirect it also removes a bounce through host memory.

`payload_bytes_copied` counts every one of those bytes. Mapping removes the
network, not the copy, and a path reporting zero here would look like the
in-place one it is not.

## Three things that are not obvious

**Host memory cannot take this path at all.** Memory the caller allocated has
no handle another process could map, and `process_vm_readv` -- the usual way
around that -- is refused between unrelated processes wherever
`kernel.yama.ptrace_scope` is 1, which is the default on all three hosts here.
So registration returns `kUnsupported` rather than staging through a shared
buffer, which would be a different path wearing this one's name. Device memory
is what this path carries.

**Being on one host is not enough.** Two devices from different vendors share
no handle they both understand, and a handle from another machine names
nothing here. The host check is made in the handshake, where it cannot be
taken on a caller's word, and the vendor check is the backend's refusal to
export.

**Releasing a region has to wait for the peer.** The mapping outlives the
registration: an allocation freed underneath one leaves the peer reading
memory that has been handed to something else. So a withdrawal is sent and the
confirmation is waited for, and if the peer has stopped polling the wait times
out and says so rather than hanging inside a deregistration -- the caller then
knows the memory is not yet safe to free.

That last point exposed something in the engine. The registration cache holds
a registration after its handle is deregistered, which is the point of the
cache and is safe on the network path as long as the memory is not reused. On
this path it means the peer is never asked to unmap, so a release that waits
would return without having waited for anything. Rather than weaken the cache,
`release_cached_registrations()` was added: deregistration stays cheap, and a
caller about to free the memory has a way to make the release real. Both
behaviours are now tested, in opposite directions.

## What was measured

102 contract cases pass, six of them on this path: the copy is reported, a
span leaving the peer's region is refused, an unpublished key is refused, two
regions inside one allocation share a mapping, a withdrawal stops the peer
using the region, and a peer that never answers produces a timeout rather than
a hang. Removing the range check fails the third; not recording the pending
confirmation fails the sixth.

End to end between two processes, on three vendors -- NVIDIA RTX 4090, Hygon
Z100L, Cambricon MLU370-X8 -- 4 MiB read and 4 MiB write each, all verified
byte for byte, the ready handoff delivered, 8 of 8 sub-operations completed,
and a release returning after 293-295 ms because the peer had been told to
hold its mapping for 300 ms before polling.

Two of the three needed something NVIDIA did not. Hygon's DTK 23.10 answers
the unified-addressing query with an error, so a capability read from that
query reported a card with no IPC support when the same card exports handles
without complaint; the question now goes to the driver. Cambricon exports only
an allocation's base -- base+4096 is refused by the driver -- which happens to
be exactly the guarantee this path needs, since that backend has no call for
finding the allocation an address belongs to and could not otherwise compute
the offset.
