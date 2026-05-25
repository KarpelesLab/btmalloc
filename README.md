# btmalloc

A research `malloc` / `free` / `realloc` replacement organized around one idea
that production allocators (jemalloc, tcmalloc, mimalloc) leave on the table:
**the call site is the primary key.** Every allocation is grouped into a
*partition* by `hash(__builtin_return_address(0))`, and placement, lifetime
cohorting, memory reclamation, and (future) security all derive from it.

See [docs/DESIGN.md](docs/DESIGN.md) for the full rationale and
[bench/RESULTS.md](bench/RESULTS.md) for measurements.

## Highlights (vs glibc and jemalloc on this machine)

- **Producer/consumer (cross-thread free): fastest of the three at every thread
  count** (2–32). A freed slot returns to its home *partition* regardless of
  which thread frees it — no per-thread-arena ownership transfer to contend on.
- **~2.7× less resident memory** after a fragmenting churn workload (72 MB
  steady / 60 MB drained, vs glibc ~190 and jemalloc ~203) — call-site cohorting
  plus empty-slab decommit return memory the others keep.
- **2× faster than jemalloc** on repeated grow/free cycles — a warm-chunk pool
  eliminates mmap/munmap thrash.
- Drop-in via `LD_PRELOAD`: runs python, git, gcc, perl, bash, and parallel
  builds cleanly.

Trade-off: the single-threaded small-object fast path trails jemalloc
(~6.7 ns vs ~2.3 ns) — the cost of hashing a call site into a cache bin instead
of indexing a per-size-class array. Wins are on scaling, cross-thread free, and
memory footprint.

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
| `BTM_ASAN` | `OFF` | Build with AddressSanitizer (forces `BTM_OVERRIDE_LIBC=OFF`) |

### Runtime knobs

- `BTM_PARTITIONS=<n>` — number of partitions (rounded up to a power of two).
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
bench/run.sh build          # compares glibc vs jemalloc vs btmalloc
```

## Status

The five build phases (PC-anchored core, lifetime cohorting + reclaim, async
io_uring backing, empty-slab decommit, LD_PRELOAD hardening) are complete and
benchmark-validated. Known future work: object-moving (Mesh-style) compaction,
live-data hotness tiering via access sampling, per-partition adaptive size
classes, and closing the single-thread small-object gap. See
[docs/DESIGN.md](docs/DESIGN.md).

## License

MIT — see [LICENSE](LICENSE).
