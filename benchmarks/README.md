# benchmarks

## hux_bench

Measures transfer time end to end.

```bash
g++ -std=c++17 -O2 hux_bench.cpp ../src/core/*.cpp ../src/control/*.cpp \
    ../src/transport/cc/*.cpp ../src/transport/local/*.cpp \
    ../src/transport/mock/mock_provider.cpp ../src/transport/rdma/rdma_provider.cpp \
    -I../include -I../src -libverbs -lpthread -o hux-bench

./hux-bench server --qp 4
./hux-bench client <ip> --qp 4 --iters 50 --sizes 4096,1048576,8388608
```

### What is timed

From submission until the request reaches a terminal state: the whole path the
caller waits on, not the network stage alone. A figure covering only the wire
looks better and answers a question nobody asked.

Each point reports a median with p10 and p90. A single number hides whether a
run was steady or lucky once. Bandwidth is derived from the median so an
outlier cannot inflate it. The first transfers are discarded — they pay for
connection setup and page faults, which are real costs but not the one being
measured.

The run prints the configuration actually in effect, both engine and provider,
so a result can be matched to the settings that produced it.

## Results so far

One host, loopback over mlx5_0, single stream, 2026-09-18. Not a cross-machine
number: loopback shares the local PCIe path, so these say more about the host
than about a fabric.

| size | read Gb/s | write Gb/s | read median | write median |
| --- | --- | --- | --- | --- |
| 4 KiB | 5.4 | 4.5 | 6.1 us | 7.4 us |
| 1 MiB | 15.8 | 14.8 | 530 us | 567 us |
| 8 MiB | 15.1 | 14.5 | 4.44 ms | 4.62 ms |

Zero bytes copied at every size: the NIC reads and writes the caller's own
memory.

### Queue pairs made no difference here

| queue pairs | 8 MiB read | 4 KiB read |
| --- | --- | --- |
| 1 | 15.13 Gb/s | 6.1 us |
| 4 | 14.94 Gb/s | 6.6 us |
| 16 | 15.27 Gb/s | 6.8 us |

Within noise on bandwidth, and slightly worse on small-message latency.

This is not evidence that multiple queue pairs are useless — it is evidence
that this workload does not need them. One stream on a loopback path has no
parallelism to exploit and no route diversity to gain: the queue pairs share a
NIC, a PCIe path and a host. Where they should matter is many concurrent
requests, a real fabric with several routes, or small messages at a rate one
queue cannot issue.

Reported because the roadmap asks for exactly this before enabling anything by
default: a queue pair count is not a count of network paths, and the benefit
has to be shown rather than assumed. Single queue pair remains the baseline
every other configuration is measured against.
