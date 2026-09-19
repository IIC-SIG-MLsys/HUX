# Where the peer is chooses the transport; what it costs is still reported

Status: decided, 2026-09-19.

## Question

An engine held one provider. A deployment with peers both in the next process
and on another machine therefore needed two engines and two registrations of
the same memory, and the roadmap asks for one set of interfaces over all three
cases. The question is what has to be true for an engine to hold several
transports without the choice becoming a place where things quietly go wrong.

## What it does

Providers are given in preference order. A peer's identity says where it is,
its metadata says which transports it offers, and the first local provider
that suits the location and appears in that list is the one used. Nothing
matches -- an IPC-only engine reaching a peer on another machine -- and the
connection is refused rather than made over something slower without saying
so.

## What that forced

**Memory is registered with every provider.** Which path a peer will arrive on
is not known when memory is registered, so all of them get a chance at it. A
provider that refuses this particular memory contributes no key, which is not
an error: IPC cannot export what the caller allocated on the host, and the
network path can. Registration fails only when none of them took it.

**A descriptor carries a key per provider.** A remote key means something only
to the transport that minted it. An importer takes the one belonging to the
path it arrived on, and a region exported only for another path is refused
where the reason is visible. Handing an rkey to a mapping would be accepted
locally and refused somewhere far away, with nothing at the point of failure
to explain it.

**Completions, control messages and statistics are per provider.** Work left
unpolled on one transport is a request that never finishes, and a byte count
taken from one provider describes a fraction of what the engine moved.

## Where the peer is, and how it was reached

`PeerCaps` reports both, because they are different questions. A peer in the
next process served over the network is a network transfer; reporting it as
local would hide exactly the fallback a caller is asking about. `place` says
where the peer turned out to be and `path` says what is carrying the bytes.

## What was measured

Five contract cases, and both negative controls land. Ignoring the peer's
location reaches a machine across the network over IPC; skipping the key
selection lets a wrong-path key through. The first of those exposed a real
defect while failing: the IPC handshake had no timeout, so a peer that never
accepted left `connect` blocked for ever. It is bounded now and reports a
timeout, which is what the rest of this library does with a wait that cannot
finish.

Not yet measured: both transports carrying real traffic from one engine at the
same time. The choice is tested with a real IPC provider and a mock network
one, which exercises the selection but not two live fabrics at once.
