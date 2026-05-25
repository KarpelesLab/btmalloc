# btmalloc — Design

> **This is a research allocator.** The goal is not to re-implement jemalloc,
> tcmalloc, or mimalloc, but to explore a design organized around a single
> idea that production allocators have not combined: **the call site is the
> primary organizing key.** Everything else — placement, lifetime grouping,
> security segregation, temperature, and async backing-store management —
> derives from it.

## Thesis

A `malloc` call carries one nearly-free, highly predictive feature that
mainstream allocators throw away: **the return address of the caller**
(`__builtin_return_address(0)`). Two decades of research (Barrett & Zorn 1993;
the 1996 lifetime-prediction work; Cling 2010; LLAMA ASPLOS 2020; SeMalloc
CCS 2024) repeatedly confirm that the call site predicts an allocation's
**size**, its **lifetime**, and a useful **type-segregation boundary**.

Existing systems exploit this only partially and expensively:

- **Cling / SeMalloc** segregate by call site for *security*, paying
  46–247 % memory overhead.
- **LLAMA** groups by predicted *lifetime*, but runs a neural net per
  allocation and needs offline training.

btmalloc unifies these roles using the raw return address as the only feature,
with **no ML, no compiler pass, no profiling run**. The mechanism is a tunable
number of **partitions**:

```
partition = hash(return_address) mod P
```

`P` is the central research knob:

| P | Behavior |
|---|----------|
| `1` | Degenerates to an ordinary size-class allocator (no anchoring). |
| moderate (64–1024) | Bounded metadata; most of the lifetime/locality benefit; collisions merge unrelated call sites. |
| very large (≈ #call-sites) | Approaches SeMalloc-style per-call-site segregation (full security, higher memory). |

Sweeping `P` lets us *measure* the value of call-site anchoring as a continuous
quantity — itself a contribution, since prior work only reports the extremes.

## Three pillars

### 1. PC-anchored partitioning (Phase A)

- On `btm_malloc(size)`: capture the return address, compute
  `sc = size_to_sc(size)` and `part = hash(ra) mod P`.
- Each `(partition, size_class)` owns a pool of **slabs**. A slab is a run of
  pages carved from a 2 MiB **chunk**, dedicated to one size class and tagged
  with its owning partition.
- A small **per-thread cache** of recently-used bins sits in front, so the hot
  path is "hash, probe one cache line, pop a freelist" — no lock, no atomic.
- Cross-thread frees route to the owning slab via a per-slab remote-free queue
  (BatchIt-style, ISMM 2024), drained by the owner.

Because placement is by call site, not by thread, objects that are logically
related land together regardless of which thread allocated them — the opposite
of the thread-local arena model, and a better fit for producer/consumer and
work-stealing workloads.

### 2. Lifetime cohorting + bulk reclaim (Phase B)

Call sites tend to allocate objects that **die together** (a request handler,
a parse of one document, one frame of a render loop). With per-partition slabs,
a partition's slabs therefore tend to drain *all at once*. We track a per-slab
live count; when it hits zero the whole slab is reclaimed in one operation
instead of being slowly picked apart. This is where the anchoring pays off in
**RSS over time**: fragmentation is suppressed structurally rather than fought
after the fact.

### 3. Async backing-store via io_uring (Phase C)

Every slow-path kernel interaction an allocator makes — `mmap` a new chunk,
`munmap` a drained one, `madvise(MADV_PAGEOUT)` a cold slab,
`madvise(MADV_POPULATE_WRITE)` a hot one — is normally **synchronous**, and is
the dominant tail-latency tax in latency-critical services (Adios, EuroSys
2025). btmalloc routes these through an **io_uring** submission queue so user
threads never block on them:

- Chunk pre-mapping runs ahead of demand (a background watermark keeps a few
  free chunks staged).
- Reclaim (`munmap`, `MADV_PAGEOUT`) is fire-and-forget.
- The backend is abstracted behind `btm_aio_*` with two implementations:
  a **raw-syscall** backend (default; no runtime dependency, no risk of
  recursing into our own malloc under `LD_PRELOAD`) and an optional
  **liburing** backend (convenience for the prefixed-API library and benches).

### 4. Hotness-aware tiering + compaction (Phase D)

Hotness fragmentation (HADES/OBASE 2025–2026: up to 97 % of bytes on active
pages are cold, yet a single hot object pins the page in DRAM) is attacked
directly:

- Per-slab access tracking (sampled, cheap) yields a temperature.
- Cold slabs are demoted with `MADV_COLD` / `MADV_PAGEOUT` (async, Phase C).
- Sparse slabs of the same `(partition, size_class)` are **compacted**
  Mesh-style: two slabs whose live-slot bitmaps don't overlap are merged via
  page remapping, freeing one slab's physical pages. Restricting merges to a
  single partition preserves the call-site segregation boundary.

### 5. Adaptive size classes (Phase E)

A partition observes the actual sizes its call sites request and tunes its slab
slot size to fit — eliminating the internal fragmentation that fixed,
allocator-wide size-class tables impose. Because a call site's size
distribution is usually narrow, per-partition slot sizes can be far tighter
than a global table.

## Memory layout

```
chunk (2 MiB, 2 MiB-aligned via mmap-2x-and-trim, MADV_HUGEPAGE)
├── page 0: chunk header  { magic, partition map, page→slab table }
└── pages 1..511: slabs (each slab = run of N pages for one (part, sc))
        slab header: { magic, owner partition, size_class, live_count,
                       freelist head, remote-free queue head, bitmap, temp }

large allocation (> 16 KiB): standalone mmap + 32-byte magic header,
        realloc via mremap(MREMAP_MAYMOVE)
```

Any pointer is resolved by masking to its 2 MiB chunk base and reading the
chunk header (the standard alignment trick), then indexing the page→slab table.

## Hot path (target)

```c
void *btm_malloc(size_t size) {
    void   *ra   = __builtin_return_address(0);
    unsigned sc  = size_to_sc(size);                 // small: branchless table
    unsigned key = mix(ra, sc);
    btm_bin *bin = &tls->cache[key & (TLS_CACHE - 1)];
    if (likely(bin->key == key && bin->free))         // hit
        return pop(bin);                              // no lock, no atomic
    return btm_malloc_slow(ra, sc, size);             // refill / large / init
}
```

## Concurrency model

- **Hot path:** per-thread bin cache → zero synchronization.
- **Refill / flush:** per-`(partition, size_class)` lock, bulk transfer.
- **Cross-thread free:** lock-free push to the owning slab's remote-free queue;
  drained by the owning context. No lock taken by the freeing thread.
- **Background:** a maintenance context (or io_uring completion handling) does
  reclaim, compaction, and tiering off the critical path.

## What we will measure (every phase)

| Axis | Benchmark | Compared against |
|------|-----------|------------------|
| Throughput (ns/op) per size class | `bench_micro` | glibc, jemalloc |
| Multi-thread scaling 1→32 | `bench_threaded` | glibc, jemalloc |
| Producer/consumer (cross-thread free) | `bench_threaded` | glibc, jemalloc |
| RSS / fragmentation over time | `bench_rss` | glibc, jemalloc |
| malloc latency p50/p99/p999 | `bench_latency` | glibc, jemalloc |
| Internal fragmentation (req vs slot) | `bench_frag` | fixed size classes |
| Real programs | `LD_PRELOAD` ls/bash/build | glibc, jemalloc |

The harness (Phase 0) is built **before** the allocator so every change is a
measured delta, not a guess.

## Build phases

- **M0** ✔ — CMake skeleton + dumb mmap-per-alloc baseline (commit ad48755).
- **Phase 0** ✔ — benchmark + profiling harness; baseline glibc/jemalloc numbers.
- **Phase A** ✔ — PC-anchored partitioned core (replaces M0). Correct
  (shadow-map stress + ASan clean); competitive throughput; **wins
  producer/consumer cross-thread-free at every thread count** (see
  bench/RESULTS.md). Open: small-object single-thread fast path trails jemalloc.
- **Phase B** ✔ — per-pool chunks, slab recycling, whole-chunk reclaim
  (active chunk MADV_DONTNEED+reset, others munmap). Returns more memory to the
  OS on drain than glibc/jemalloc (bench/RESULTS.md). Open: whole-chunk reclaim
  is pinned by single cached slots — Phase D compaction addresses this.
- **Phase C** ✔ — warm-chunk pool recycles drained chunks; background thread
  releases pages via batched io_uring MADV_DONTNEED and pre-maps ahead of
  demand. RSS drained 136 MB (best of three); 2× faster than jemalloc on
  grow/free thrash. Malloc tail is refill-bound (separate optimization).
- **Phase D** ✔ — empty-slab decommit: empties beyond a small warm budget have
  their pages released (MADV_DONTNEED) instead of pinning a chunk. RSS after a
  fragmenting churn drops to ~72 MB steady / 60 MB drained vs glibc 190 /
  jemalloc 203 — ~2.7× less. (Object-moving compaction + live-data MADV_COLD
  tiering remain future work — see bench/RESULTS.md.)
- **Phase E** ✔ — LD_PRELOAD hardening: pthread_atfork child handler, extra
  libc stubs, real-program validation (python/git/gcc/perl/threaded). Adaptive
  size classes deferred (per-partition size tables would tax the hot path for a
  marginal frag gain) — see bench/RESULTS.md.

Each phase ends with a green build, passing tests, refreshed benchmark numbers,
and a commit pushed to `master`.

## Platform assumptions (verified 2026-05-25)

Linux 6.12, x86_64, glibc 2.41, GCC 14. i9-14900K (32 logical cores, P+E
hybrid). THP = `madvise`. io_uring fully available. `MADV_COLD`, `MADV_PAGEOUT`,
`MADV_POPULATE_READ/WRITE` all present. jemalloc available as a comparison
baseline; mimalloc/tcmalloc not installed (may build from source later).

## Open questions (tracked, resolved by measurement)

1. Best `P`, and whether it should be static, set by env, or auto-tuned.
2. TLS cache geometry (entries, associativity, eviction) for real call-site
   working-set sizes.
3. Whether wrappers (`xmalloc`) defeat depth-0 anchoring badly enough to need
   `__builtin_return_address(1)` or a configurable depth.
4. Strict (true per-call-site, security) vs relaxed (partitioned) — quantify the
   memory/security trade-off on one codebase.
5. io_uring submission batching granularity vs latency.
