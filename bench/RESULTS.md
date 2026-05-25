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

## Phase B — lifetime cohorting + bulk reclaim

Per-(partition, size_class) chunks; fully-free slabs recycled; whole drained
chunks released (active chunk via MADV_DONTNEED+reset, others via munmap).

### RSS over a grow → churn → shrink → drain workload (MB, lower better)

`bench_rss 200000 3000000`, single call site (→ one partition):

| alloc    | peak | shrunk | steady | drained |
|----------|------|--------|--------|---------|
| glibc    | 182  | 191    | 190    | 187     |
| jemalloc | 187  | 203    | 203    | 203     |
| btmalloc | 184  | 248    | 184    | **148** |

**btmalloc returns the most memory on drain** (148 vs 187 vs 203) — glibc and
jemalloc essentially never give it back. Hot path (churn) and the
producer/consumer win are unchanged from Phase A.

Limitation (motivates Phase D): reclaim is whole-chunk, so a single bin-cached
slot pins its slab, and one pinned slab pins its entire 2 MiB chunk. Under
random churn this leaves chunks partially live (the mid-shrink 248 MB spike and
the 148 MB residual). Phase D compaction consolidates sparse slabs so chunks
can fully drain.

## Phase C — async backing-store (warm-chunk pool + io_uring madvise)

Global warm-chunk pool recycles drained chunks; a background thread releases
their pages with batched `io_uring` `MADV_DONTNEED` and pre-maps ahead of
demand. `BTM_NO_ASYNC=1` forces the synchronous fallback.

### RSS drained (MB, lower better)

| alloc | peak | drained |
|-------|------|---------|
| glibc | 182 | 187 |
| jemalloc | 187 | 203 |
| btmalloc | 184 | **136** |

Improved from Phase B's 148 — async page release reclaims more.

### Thrash: 40× (grow to 50k objects → free all), wall time (lower better)

| alloc | time |
|-------|------|
| glibc | 32.2 ms |
| jemalloc | 63.6 ms |
| btmalloc | 32.9 ms |
| btmalloc (BTM_NO_ASYNC) | 33.4 ms |

btmalloc matches glibc and is **2× faster than jemalloc** — the warm pool
eliminates the mmap/munmap cycling this pattern would otherwise cause.

### Malloc latency (size=256, 16384-slot working set; ns, lower better)

| pct | glibc | jemalloc | btmalloc |
|-----|-------|----------|----------|
| p999 | 33 | 37 | **23** |
| p9999 | 1237 | **114** | 1031 |

btmalloc wins p999 but its p9999 trails jemalloc: the malloc tail is dominated
by the locked **refill** path (bin-empty → slab carve), which the warm pool
doesn't touch. Flattening it is a refill-path optimization, separate from
async backing. Async helps the *free* path (madvise offloaded) — not visible in
a malloc-latency bench.

## Phase D — empty-slab decommit (fragmentation RSS)

When a slab empties but its chunk still has survivors, all but a few warm
empties are decommitted (`MADV_DONTNEED` on their data pages) and parked on a
cold list; reuse re-faults them. This releases the memory of empty slabs that
whole-chunk reclaim could not, because one survivor pinned the chunk.

### RSS over grow → churn → shrink → drain (MB, lower better)

| alloc    | peak | shrunk | steady | drained |
|----------|------|--------|--------|---------|
| glibc    | 182  | 191    | 190    | 187     |
| jemalloc | 187  | 203    | 203    | 203     |
| btmalloc | 184  | **140**| **72** | **60**  |

**btmalloc uses ~2.7× less resident memory than glibc/jemalloc** after a
fragmenting churn — steady 72 MB vs ~190/203, drained 60 MB vs 187/203.
Throughput unchanged (churn ~6.9 ns; producer/consumer still wins; local
scaling unaffected).

Scope: this is empty-slab decommit, not object-moving compaction. 1-page
small-class slabs can't be decommitted individually (inline header shares the
page) and still rely on whole-chunk reclaim. True Mesh-style compaction (merge
sparse live slabs) needs memfd-backed chunks + virtual remapping — future work.
Live-data hotness tiering (MADV_COLD on cold-but-live slabs) needs access
sampling — also future work.

## Phase E — LD_PRELOAD hardening

`pthread_atfork` child handler (reinitializes pool locks and resets the
background subsystem, since only the forking thread survives into the child);
extra libc compatibility stubs (`cfree`, `malloc_trim`, `malloc_stats`); a
CTest (`test_preload`) that runs real programs under the built `.so` and
exercises fork+child allocation.

Validated as a drop-in (LD_PRELOAD confirmed active via /proc/self/maps):
`/bin/ls`, `/bin/bash`, `/usr/bin/find`, `grep`, **python3** (JSON + 8-thread
workloads), **git**, **gcc** (compiling btmalloc's own sources), **perl**, and
**parallel gcc** — all run cleanly. AddressSanitizer clean across the suite.

Deferred (future work, with rationale): per-partition **adaptive size classes**
would tune internal fragmentation to each call site's observed sizes, but
require a per-partition size→class table on the hot path (replacing today's
single shared table read) — a real throughput cost for a marginal frag gain,
not justified while the design's headline wins (cross-thread free, RSS) are
already demonstrated. True object-moving compaction (Mesh-style, memfd-backed)
and live-data MADV_COLD tiering (access sampling) likewise remain future work.

## Takeaways

- **Win to defend:** cross-thread free / producer-consumer scaling.
- **Gap to close:** single-thread small-object fast path (inline the bin
  lookup; consider LTO; trim the RA hash). Tracked as Phase A tuning.
- Phases B (bulk reclaim), C (async backing), D (compaction) target RSS and
  tail latency, measured by `bench_rss` and `bench_latency`.
