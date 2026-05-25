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

## Phase I — security hardening (freelist safe-linking + call-site forensics)

btmalloc now stores every free slot's next-pointer XORed with a per-process
random secret (seeded from getrandom) and the slot's own page address
(safe-linking, as in glibc 2.32+ and mimalloc). A heap overflow that overwrites
a freed object's link can no longer steer the allocator to an attacker-chosen
address — the corrupted value decodes to an unpredictable, unusable pointer.

Combined with `BTM_PARTITION_MODE=intern` (deterministic per-call-site
segregation, which blocks cross-type reuse) and the call-site heap profiler
(`btm_heap_profile`, which can name the call site that allocated a corrupted
slab — forensics no size/thread-keyed allocator can provide), btmalloc now has
a coherent exploitation-mitigation story.

Cost: ~0 — one XOR folded into each freelist load/store; churn is unchanged
(~6.7 ns). Verified: `test_harden` confirms freed slots store an obfuscated
link, not the raw next address; round-trip correctness is covered by the
millions-of-ops shadow-map stress test running with safe-linking active; ASan
clean; git/python/bash run under LD_PRELOAD.

`BTM_HARDEN=1` additionally enables **double-free detection**: a freed slot
stamps a key-derived canary (cleared on reallocation); freeing a slot that
still carries it is a consecutive double-free, which aborts with **call-site
forensics** — the report names the call site feeding the corrupted slab's
partition (unique to PC anchoring). Cost ~0.8 ns/op (opt-in; default path
untouched). `test_doublefree` confirms detection (child aborts on SIGABRT) and
no false positives on legitimate churn.

(Building the double-free path surfaced and fixed a latent correctness bug: the
size-class lookup table was read before lazy init had built it, so the very
first allocation of a process returned a 16-byte slot regardless of requested
size — a heap overflow for any first allocation > 16 B. The table is now a
compile-time constant, valid before any init.)

## Phase H — out-of-line slab metadata (and its RSS payoff)

Slab descriptors were moved out of the data region into a per-chunk descriptor
array, so data slabs are now pure slot-only pages. This was done to unblock
meshing (Phase G), but it had a large independent payoff: **empty-slab decommit
(Phase D) now works for small, single-page classes too** — the old inline
header shared the page with slots and blocked it.

### RSS over grow → churn → shrink → drain (MB, lower better) — updated

| alloc    | peak | steady | drained |
|----------|------|--------|---------|
| glibc    | 182  | 190    | 187     |
| jemalloc | 187  | 203    | 203     |
| btmalloc | 183  | **29** | **19**  |

btmalloc now holds **~6.5x less** resident memory than glibc and **~7x less**
than jemalloc at steady state (and ~10x less drained) on this fragmenting
workload — up from the ~2.7x of the inline-header layout. Throughput is
unregressed (churn ~7 ns; producer/consumer still wins, 8t 8.1 vs glibc 6.1 /
jemalloc 5.6 Mops). Mesh mode (BTM_MESH) now adds zero peak overhead, so
btm_compact() is net-positive on top of this.

(Earlier Phase B/C/D numbers below were taken with the inline-header layout.)

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

## Phase F — hot-path simplification (dense bin cache)

Replaced the hashed, eviction-based per-thread bin cache with a dense array
indexed directly by `partition * NUM_SC + size_class` (mmap'd, so untouched
bins never fault in — RSS is unchanged). Stored the partition index in the slab
to drop a per-free pointer-subtraction-by-non-power-of-two (a hidden
reciprocal-multiply), and resolve a freed slot's pool via `chunk->pool`
directly.

Effect: tight-churn throughput is unchanged (~6.8 ns) — the remaining gap to
jemalloc (~2.2 ns) is **fixed per-op overhead from header-free pointer
resolution** in `free` (several dependent metadata loads), structural to a
design that stores no per-object header. The win is for real programs with
many call sites: the old hashed cache could thrash (evict + re-flush) once the
active `(partition, size_class)` set exceeded the table or collided; the dense
array never evicts. The code is also simpler (no hashing, no eviction, no
key bookkeeping).

## Fast-path investigation (where the small-object gap actually is)

Measured directly (static link, no LD_PRELOAD) to isolate costs:

- **direct churn: 6.0 ns/op** vs ~7 ns through LD_PRELOAD → the preload wrapper
  (PLT → shim → core, with the return-address capture) costs ~1 ns.
- The allocator path itself is ~6 ns vs jemalloc's ~4.4 ns. Successively
  removing the bin hash + eviction (dense index), a per-free pointer-division
  (stored partition index), and the small/large magic load (owner low-bit tag)
  each left churn essentially unchanged — confirming the residual ~1.5 ns is
  **fixed per-op work inherent to the design**: hashing the call site into a
  partition, and resolving a freed pointer through its slab metadata with no
  per-object header. These are the cost of the features that win elsewhere
  (call-site grouping, cross-thread-free routing, low RSS).

Conclusion: the small-object single-thread gap is a small constant, not an
algorithmic one, and it is amortized in real workloads that do actual work per
allocation. Not worth trading the design's wins to chase further.

## Partition sweep on a real workload (peak RSS vs P)

`bench/sweep_partitions.sh` — a multi-call-site python workload (dicts, lists,
strings, JSON; held working set with churn), peak RSS via `getrusage`:

| config        | peak RSS | wall  |
|---------------|----------|-------|
| glibc         | 356.7 MB | 5.93s |
| btmalloc P=1  | 372.3 MB | 6.28s |
| btmalloc P=8  | 374.5 MB | 6.25s |
| btmalloc P=64 | 375.8 MB | 6.22s |
| btmalloc P=256| 376.5 MB | 6.24s |
| btmalloc P=1024| 378.3 MB | 6.27s |

Honest reading:

- **btmalloc's RSS advantage is on reclamation, not peak.** On a workload that
  *holds* a working set, btmalloc is ~5% heavier than glibc (per-partition
  minimum overhead). The big win (72/60 MB vs ~190/203) is on *returning*
  memory after a burst, which glibc/jemalloc largely don't do.
- **P costs little at peak and is roughly throughput-neutral here** (372→378 MB
  over P=1→1024). P's *benefits* — call-site cohorting for reclamation and
  segregation for security — need workloads/metrics that stress them; a
  general held-working-set workload doesn't, so raising P is nearly free.
- **Throughput is within ~5% of glibc** on a real application.

This is the trade-off profile of the design: wins on cross-thread free and
post-burst memory return; roughly neutral-to-slightly-behind on single-thread
throughput and peak footprint of a held working set.

## Deterministic call-site interning (BTM_PARTITION_MODE=intern)

Default partition selection is `hash(return_address) mod P` — statistical
segregation, where distinct call sites can collide from the start. Intern mode
assigns each distinct call site its own partition (until P is exhausted, then
graceful wrap), giving *deterministic* per-call-site segregation: a freed slot
is only ever reused by allocations from the same call site, until P fills.

This is both a security property (the Cling/SeMalloc anti-type-confusion
guarantee) and finer profiling. Measured trade-off (the survey notes prior work
only reports the extremes):

- **Segregation/profiling granularity** (python workload, P=64): intern
  separates **54** distinct call-site groups vs hash's **37** (collisions);
  scales further with P.
- **Memory cost**: at P=256, intern ties hash (426 MB — fewer call sites than
  partitions, so no extra spreading). At P=4096, intern is +14 MB (~3%) to give
  each site its own partition — vs SeMalloc's reported 46–247%.
- **Throughput**: intern adds ~2 ns/malloc (two TLS-cache loads + the mode
  test) — 8.5 vs 6.6 ns churn. **Opt-in**: the default hash path is unchanged.

So the design exposes a tunable knob from "fast, statistical segregation"
(default) to "deterministic per-call-site segregation" (intern + large P) at a
small, *measured* memory cost — the quantified trade-off curve prior work left
implicit.

## Phase G — true Mesh-style compaction (opt-in BTM_MESH=1)

Implements object-moving compaction the Mesh (PLDI'19) way: chunks are
memfd-backed, and `btm_compact()` finds sparse slabs of the same size class
whose live-slot sets are disjoint, copies one slab's live objects into the
other's matching free slots, and remaps the donor's virtual pages onto the
recipient's physical pages (MAP_FIXED) — so objects move in *physical* memory
while their *virtual* addresses (and thus all pointers) stay valid. The donor's
own pages are punched out. Requires a quiescent heap (caller's responsibility);
trivially safe single-threaded.

**The mechanism is correct.** A shadow-map test (`test_compact`) allocates
120k medium objects, frees 85% at random, compacts, and verifies all ~17.9k
survivors are byte-intact after their physical pages were relocated — clean,
including under AddressSanitizer.

**Net-positive after the out-of-line refactor (Phase H).** With data slabs now
pure pages, mesh mode adds no peak overhead, and compaction recovers memory on
top:

| same workload (alloc 120k, free 85%) | RSS after |
|--------------------------------------|-----------|
| default mode                         | 191.9 MB |
| mesh mode, before compact            | 191.9 MB |
| mesh mode, after compact             | 187.6 MB |

Mesh mode now matches default at peak (zero density penalty) and btm_compact()
reclaims 4.2 MB on top. Meshing's absolute win is still workload-dependent (it
needs disjoint sparse pairs), but it is no longer paid for with overhead.
Fork-unsafe (MAP_SHARED, MADV_DONTFORK'd); see DESIGN.

## Phase J — live-data cold tiering (btm_pageout_cold)

Attacks "hotness fragmentation" (HADES/OBASE 2025-26: cold-but-live data pins
DRAM). Each slab carries an activity epoch stamped on allocator activity (slow
paths only — zero hot-path cost). `btm_pageout_cold()` evicts the data pages of
*settled* slabs — full, and untouched since the previous call — to swap with
`MADV_PAGEOUT`. Objects stay valid and fault back transparently on next access;
the allocator-activity epoch is the coldness proxy, and the per-partition
structure means a quiet call site's whole cohort ages out together.

### Demonstration (60 MB cold dataset + a churning hot set)

| | RSS |
|---|---|
| before `btm_pageout_cold()` | 63.6 MB |
| after  | **4.0 MB** |

A **94% resident drop** — cold live data evicted to swap — with all 120k cold
objects verified byte-intact afterward (they fault back) and the hot set
untouched. Works in default and mesh modes; requires swap to reduce RSS (the
mechanism + safety hold regardless). No throughput regression (churn ~6.7 ns).

## Takeaways

- **Win to defend:** cross-thread free / producer-consumer scaling.
- **Gap to close:** single-thread small-object fast path (inline the bin
  lookup; consider LTO; trim the RA hash). Tracked as Phase A tuning.
- Phases B (bulk reclaim), C (async backing), D (compaction) target RSS and
  tail latency, measured by `bench_rss` and `bench_latency`.
