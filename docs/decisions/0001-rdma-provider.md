# Do not adopt UCCL as the RDMA provider

Status: decided, 2026-09-18.
Measured against UCCL `main` as vendored in `uccl_try/uccl`, on a single host
with two processes on one NVIDIA A40 over mlx5_3.

## Question

The roadmap notes that UCCL P2P could serve as HUX's RDMA provider. Adopting
it would remove the largest block of work ahead -- the multi-QP engine and
congestion control -- so the choice is worth settling before either path is
built, rather than carrying two transport engines for months.

Four conditions were set in advance. All had to hold:

1. a batched read completes with its data verified
2. failures map onto `ErrorInfo`, distinguishing would-block, timeout and
   unsupported
3. registration, connection and request lifetimes line up
4. a self-written provider for another vendor can coexist under one engine

## What was measured

**1. Normal path: passes.** A 4 MiB read over RDMA completed after 9350 polls
and the data verified byte for byte. Transfers work and they are not slow.

**2. Failure path: fails, and not in a way a wrapper can repair.** A read
issued against a bogus remote address produces, in the RDMA layer:

```
CQE error, status=10 (remote access error), vendor_err=0x88, qp_num=0x6f4
WR_FLUSH_ERR, qp_num=0x6f4
```

The application sees none of it. `poll_async` neither reports an error nor
ever returns done; the caller waits forever. The probe was killed by a 120 s
timeout. `bool poll_async(uint64_t, bool*)` has no error field at all, so
there is nowhere for a reason to surface even in principle.

This is the disqualifying result. HUX defines `failed_safe` as "the request
failed and the related local DMA has stopped or been isolated", and every
error path drains to a terminal state before resources are released. A wrapper
can impose its own timeout, but it cannot learn whether the NIC is still
reading the source buffer, so it can never honestly reach `failed_safe` -- it
would have to either free memory that may still be under DMA, or keep every
failed request pinned forever.

**3. Completion state is single-use.** Querying a completed transfer a second
time aborts the process:

```
completed after 9350 polls
data moved: True
querying the same transfer a second time ...
free(): double free detected in tcache 2
```

`poll_async` deletes its `TransferStatus` the first time it reports done, and
`transfer_id` is that pointer. HUX requires repeated `test`/`wait` to return a
consistent answer. A wrapper could cache the outcome and stop polling after
the first completion, so on its own this is survivable -- it is recorded
because it shows the completion model is built around one-shot handles.

**4. Not reached.** Condition 2 already settles the question.

## Other gaps found on the way

* `FifoItem.size` is `uint32_t`, capping a single transfer below 4 GiB. HUX
  requires 64-bit offsets and lengths.
* `submit` has no notion of partial acceptance, so `SubmitResult.accepted`
  cannot be filled honestly: a caller cannot learn how many sub-operations the
  provider took.
* The Python binding takes a PCI BDF where the C++ `connect` takes a GPU
  index. Harmless in itself, but the two surfaces do not agree.

## Decision

Write the RDMA provider. UCCL remains worth reading closely -- its batching,
QP handling and its hard-won vendor workarounds are a better starting point
than a blank page, and its Cambricon notes match what this project measured
independently. It is the error and lifetime model that does not fit, not the
transport.

This is a judgement about UCCL behind *this* contract. Under its own usage --
Python, Ray, paths known to succeed -- it does what it sets out to do.

## Cost

The estimate moves from roughly 5-6 months to 7-9 months for a team of four,
the difference being NET-01, NET-02, CC-01 and CC-02.

## Reproducing

`tests/manual/uccl_contract_probe.py`, two processes on one host:

```bash
python uccl_contract_probe.py acceptor
python uccl_contract_probe.py initiator <ip>
```

Both endpoints must sit on a GPU that supports GDR. On this host the RTX 4090s
do not (`CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED = 0`, `ibv_reg_mr` returns
`EFAULT` at every size) while the A40 does, over both the peer-memory and the
DMA-BUF path.
