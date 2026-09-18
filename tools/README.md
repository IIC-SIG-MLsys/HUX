# tools

## hux_topology

Which NIC each accelerator should use. On a machine with several of both, the
pairing decides whether a transfer crosses a socket, which outweighs most
other tuning.

```bash
g++ -std=c++17 hux_topology.cpp ../src/device/topology.cpp -o hux-topology \
    -I../src -I../include -DHUX_TOPO_CUDA -I/usr/local/cuda/include -lcudart
./hux-topology            # human readable
./hux-topology --json     # for a run to record
```

`-DHUX_TOPO_ROCM` and `-DHUX_TOPO_NEUWARE` cover the other vendors. Without
any of them it reports NICs only and needs nothing installed.

`cross_numa` is worth acting on. `unknown` means the machine did not say --
not that the devices are far apart.

## probe_registration

Asks whether an accelerator's device memory can be registered for RDMA, and up
to what size. Run it first on any new hardware: whether a backend can transfer
in place, or has to stage through pinned host memory, follows from the answer,
and vendor source does not reveal it.

Build for the vendor under test:

```bash
# NVIDIA
g++ -std=c++17 probe_registration.cpp -o probe -DHUX_PROBE_CUDA \
    -I/usr/local/cuda/include -lcudart -libverbs

# Moore Threads
g++ -std=c++17 probe_registration.cpp -o probe -DHUX_PROBE_MUSA \
    -I$MUSA_HOME/include -L$MUSA_LIB -lmusart -lmusa -libverbs

# AMD / Hygon
g++ -std=c++17 probe_registration.cpp -o probe -DHUX_PROBE_ROCM \
    -I$ROCM_PATH/include -L$ROCM_PATH/lib -lamdhip64 -libverbs
```

### Results so far

All measured 2026-09-18, sizes from 4 KiB to 512 MiB.

| Device | NIC | Device memory | Pinned host memory |
| --- | --- | --- | --- |
| Hygon Z100L (DTK) | rocep227s0f0 | OK at every size | OK at every size |
| Cambricon MLU370-X8 (Neuware) | rocep10s0f0 | OK to 256 MiB | `cnMallocPeerAble`: `EINVAL` at every size |
| Moore Threads S3000 (MUSA 3.1.0) | mlx5_0 | `EFAULT` at every size | OK at every size |

Three findings worth keeping:

* **Moore Threads cannot register device memory at all.** Even 4 KiB is
  refused, so that backend has to stage through pinned host memory. HMC's
  Moore path allocates host memory with `musaMalloc` commented out, which this
  confirms was necessary rather than an oversight.
* **Cambricon's limit is per process, not per call.** A single 256 MiB region
  registers and 320 MiB does not, yet four separate 64 MiB regions already
  exhaust the same quota. A caller checking only a per-call limit will still
  fail once several regions are live. This also explains earlier reports of a
  ceiling "around 24-44 MiB that moves with fragmentation": the quota was
  simply already partly spent.
* **`cnMallocPeerAble` is the wrong call despite its name.** Its memory is
  rejected at every size; plain `cnrtMalloc` registers fine.
