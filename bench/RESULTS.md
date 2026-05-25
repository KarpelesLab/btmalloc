# Benchmark results

Machine: Intel i9-14900K (8 P-cores + 16 E-cores, 32 threads), Linux 6.12,
glibc 2.41, GCC 14. Comparison baselines: glibc malloc, jemalloc 5.x
(`/usr/lib64/libjemalloc.so.2`). All runs via `bench/run.sh` with
`BENCH_TASKSET` pinning to reduce P/E-core variance.

Numbers are indicative single runs, not averaged — treat as deltas, not
absolutes. Reproduce with `bench/run.sh`.

## Phase A — PC-anchored partitioned core (P=64 default)

### Single-thread throughput, `micro_churn` (ns/op, lower better)

| size  | glibc | jemalloc | btmalloc |
|-------|-------|----------|----------|
| 16    | 5.0   | **2.1**  | 7.1      |
| 64    | 4.2   | **2.3**  | 7.1      |
| 1024  | 4.3   | **3.2**  | 7.6      |
| 4096  | 14.7  | **4.6**  | 7.1      |
| 16384 | 16.1  | 17.8     | **6.7**  |

btmalloc is a flat ~7 ns across sizes (wins on large; ~2-3x behind jemalloc on
small). The flat cost is hot-path overhead (cross-TU bin lookup, RA hash) —
optimization target.

### Latency, size=64 (ns, lower better)

| pct    | glibc | jemalloc | btmalloc |
|--------|-------|----------|----------|
| p50    | ~11   | ~11      | ~13      |
| p99    | 12    | 13       | 14       |
| p999   | 19    | 33       | **15**   |
| p9999  | 40    | 81       | 96       |

### Multi-thread `thr_local` (Mops/sec, higher better)

| threads | glibc | jemalloc | btmalloc |
|---------|-------|----------|----------|
| 1       | 95    | **122**  | 76       |
| 4       | 359   | **453**  | 293      |
| 16      | 989   | **1296** | 558      |
| 32      | 757   | **995**  | 675      |

### Multi-thread `thr_prodcons` — cross-thread free (Mops/sec, higher better)

| threads | glibc | jemalloc | btmalloc |
|---------|-------|----------|----------|
| 2       | 10.2  | 13.3     | **17.0** |
| 4       | 7.4   | 7.9      | **11.0** |
| 8       | 7.1   | 6.3      | **9.5**  |
| 16      | 6.5   | 5.0      | **8.1**  |
| 32      | 6.7   | 4.7      | **7.4**  |

**btmalloc wins the producer/consumer pattern at every thread count** — the
central prediction of the PC-anchored design. Per-thread-arena allocators
(glibc, jemalloc) degrade when one thread frees another's memory; btmalloc's
partition placement routes a freed slot back to its home partition regardless
of which thread frees it, with no arena-ownership transfer. (Note: this bench
is itself queue-bottlenecked, so the absolute Mops understate the allocator's
headroom; the *relative* ordering is the signal.)

## Takeaways for next phases

- **Win to defend:** cross-thread free / producer-consumer scaling.
- **Gap to close:** single-thread small-object fast path (inline the bin
  lookup; consider LTO; trim the RA hash). Tracked as Phase A tuning.
- Phases B (bulk reclaim), C (async backing), D (compaction) target RSS and
  tail latency, measured by `bench_rss` and `bench_latency`.
