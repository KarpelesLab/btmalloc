# btmalloc

A research `malloc` / `free` / `realloc` replacement organized around one idea
that production allocators (jemalloc, tcmalloc, mimalloc) leave on the table:
**the call site is the primary key.** Every allocation is grouped into a
*partition* by `hash(__builtin_return_address(0))`, and placement, lifetime
cohorting, memory reclamation, and (future) security all derive from it.

See [docs/DESIGN.md](docs/DESIGN.md) for the full rationale and
[bench/RESULTS.md](bench/RESULTS.md) for measurements.

## Highlights (vs 7 other allocators on this machine)

Benchmarked head-to-head against glibc, jemalloc, mimalloc, **tcmalloc**,
**snmalloc**, **ffmalloc**, and **hardened_malloc** (the last four built from
source by `bench/fetch_allocators.sh`). Full table in
[bench/RESULTS.md](bench/RESULTS.md).

- **Cross-thread free: tied for fastest.** Producer/consumer at 8 threads,
  btmalloc (10.3 Mops) is statistically tied with **snmalloc** (10.4) — the
  state-of-the-art message-passing allocator — and ahead of mimalloc (7.8),
  tcmalloc (6.5), and jemalloc (5.6). A freed slot returns to its home
  *partition* regardless of which thread frees it: no arena-ownership transfer
  to contend on.
- **Tightest memory footprint of all eight.** Under fragmenting churn btmalloc
  holds 2.4× live bytes (27 MB) and drains to 19 MB. *Every* mainstream
  performance allocator — glibc, jemalloc, mimalloc, tcmalloc, and snmalloc —
  sits at ~16–18× (≈190–206 MB) and never gives it back. Only dedicated security
  allocators approach it (ffmalloc 4.1×, hardened_malloc 3.5×), and far slower.
- **Faster than dedicated security allocators, at comparable memory release**
  (6.9 ns churn vs ffmalloc 14, hardened_malloc 27; 519 vs 50 Mops local).
- **Live-data cold tiering** (`btm_pageout_cold()`): evicts cold-but-live data
  to swap (94% RSS drop in the demo); objects fault back transparently.
- Drop-in via `LD_PRELOAD`: runs python, git, gcc, perl, bash, and parallel
  builds cleanly — and ran every benchmark without aborting (ffmalloc and
  hardened_malloc did not).
- **Security** (compile-time `BTM_HARDENING`, on by default; ≈3-4% churn cost):
  freelist safe-linking and double-free detection; plus optional deterministic
  per-call-site segregation (`BTM_PARTITION_MODE=intern`) and a zero-cost
  call-site heap profiler.

Trade-off: the single-threaded small-object fast path trails the performance
pack (~6.9 ns vs snmalloc 2.0 / jemalloc–tcmalloc 2.3 / mimalloc 2.8) — the cost
of hashing a call site and resolving pointers through a fault-free owner table
rather than indexing a per-size-class array. The wins are scaling, cross-thread
free, and memory footprint. `p999` tail latency is near-best (18 ns).

## How it works (one paragraph)

`hash(return_address) mod P` picks a partition. Each `(partition, size_class)`
owns slabs carved from 2 MiB chunks; a per-thread direct-mapped cache of bins,
keyed by `(partition, size_class)`, gives a lock-free hot path. Any pointer is
resolved back to its owner through a two-level radix registry over 2 MiB
regions (so `free` never dereferences unmapped memory). Drained chunks are
recycled through a global warm pool whose pages are released asynchronously by
a background thread using batched `io_uring` `MADV_DONTNEED`; empty slabs beyond
a small warm budget are decommitted to fight fragmentation. `P` is the central
knob (`BTM_PARTITIONS`, default 64): `P=1` is an ordinary size-class allocator,
large `P` approaches per-call-site segregation.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires liburing (for the async backing store) and a Linux kernel with
io_uring (5.x+; tested on 6.12).

### CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `BTM_BUILD_TESTS` | `ON` | Build the CTest suite |
| `BTM_BUILD_BENCH` | `OFF` | Build the benchmarks under `bench/` |
| `BTM_OVERRIDE_LIBC` | `ON` | Compile libc symbol overrides into the shared lib |
| `BTM_HARDENING` | `ON` | Freelist safe-linking + double-free detection. Off = plain freelists (≈3-4% faster churn, no overflow/double-free protection) |
| `BTM_PARTITIONING` | `ON` | Call-site (return-address) partitioning — the defining feature. Off = a single partition (ordinary per-size-class allocator): no return-address hash, ≈7-10% faster small churn, but no call-site segregation, RSS cohorting, or `intern`/profiling. `BTM_PARTITIONS`/`BTM_PARTITION_MODE` become no-ops |
| `BTM_OWNER_ENGINE` | `registry` | Fault-free pointer→owner resolver for `free()`: `registry` (radix + per-thread cache), `flat` (single 512 MiB lazy table, one load), or `nocache` (radix only). See [bench/RESULTS.md](bench/RESULTS.md) — `nocache` matches/beats the default on every measured workload |
| `BTM_ASAN` | `OFF` | Build with AddressSanitizer (forces `BTM_OVERRIDE_LIBC=OFF`) |

### Runtime knobs

- `BTM_PARTITIONS=<n>` — number of partitions (rounded up to a power of two).
- `BTM_PARTITION_MODE=intern` — deterministic per-call-site segregation: each
  distinct call site gets its own partition (until `BTM_PARTITIONS` is
  exhausted). Gives a Cling/SeMalloc-style anti-type-confusion guarantee (a
  freed slot is only reused by the same call site) and collision-free heap
  profiling, at a measured ~3% memory and ~2 ns/malloc cost. Default is
  `hash` (statistical segregation, faster).
- `BTM_NO_ASYNC=1` — disable the background maintenance thread (synchronous
  backing-store path).

## Using

```c
#include <btmalloc.h>
void *p = btm_malloc(1024);
btm_free(p);
```

Or as a drop-in:

```sh
LD_PRELOAD=$PWD/build/libbtmalloc.so your-program
```

### Call-site heap profiling (free, zero-instrumentation)

Because btmalloc groups allocations by call site, it can attribute live memory
to *where it was allocated* with no hooks or recompilation:

```c
btm_heap_profile(2);   /* write a per-call-site report to stderr (fd 2) */
```

Under `LD_PRELOAD`, the standard `malloc_stats()` triggers the same report, so
you can profile any program:

```c
/* from anywhere in the target, e.g. a signal handler or debugger call */
malloc_stats();
```

Output attributes outstanding bytes to symbolized call sites:

```
part   bytes_outstanding  call site
31           26618048  ~PyThread_allocate_lock (0x... in libpython3.13.so)
24              52224  ~? (0x... in libpython3.13.so)
...
```

Raise `BTM_PARTITIONS` for finer attribution.

## Benchmarking

```sh
cmake -S . -B build -DBTM_BUILD_BENCH=ON && cmake --build build -j
bench/run.sh build          # compares glibc vs jemalloc vs mimalloc vs btmalloc
```

To compare against more allocators, fetch and build them from source first —
`bench/run.sh` then auto-discovers them:

```sh
bench/fetch_allocators.sh   # builds snmalloc, tcmalloc, ffmalloc, hardened_malloc
bench/run.sh build          # now an 8-way comparison
```

See [bench/RESULTS.md](bench/RESULTS.md) for the eight-way results (btmalloc is
tied with snmalloc on cross-thread free and tightest on memory footprint).

## Status

Complete and benchmark-validated: the PC-anchored partitioned core, lifetime
cohorting + bulk reclaim, async io_uring backing, empty-slab decommit, LD_PRELOAD
hardening, out-of-line slab metadata, Mesh-style compaction (`BTM_MESH=1`),
live-data cold tiering (`btm_pageout_cold`), security hardening, the call-site
heap profiler, and a fuzzer. Resolution strategy and the two defining features
are build-time selectable (`BTM_OWNER_ENGINE`, `BTM_PARTITIONING`,
`BTM_HARDENING`); btmalloc is benchmarked against seven other allocators via
`bench/fetch_allocators.sh` + `bench/run.sh`.

Characterized but not "fixed" (they're structural, not bugs — see
[bench/RESULTS.md](bench/RESULTS.md)): the single-thread small-object gap is the
cost of the call-site machinery, not a removable bottleneck; the fault-free owner
resolve is the price of being a safe drop-in (a mask-and-trust shortcut was
measured and rejected for crashing on foreign frees). Open future work:
per-partition adaptive size classes, and a "fat" pagemap owner engine that
returns size-class metadata directly. See [docs/DESIGN.md](docs/DESIGN.md).

## License

MIT — see [LICENSE](LICENSE).
