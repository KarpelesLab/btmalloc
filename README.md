# btmalloc

A `malloc` / `free` / `realloc` replacement library in the style of jemalloc /
tcmalloc / mimalloc. Ships both a prefixed C API (`btm_malloc`, …) and an
`LD_PRELOAD` drop-in that overrides libc's malloc-family symbols.

> **Status:** under active development. The first milestone (M0) is a
> deliberately dumb mmap-per-allocation baseline that proves the build /
> test / packaging pipeline. Real performance work starts at M1.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `BTM_BUILD_TESTS` | `ON` | Build CTest test suite |
| `BTM_BUILD_BENCH` | `OFF` | Build microbenchmarks under `bench/` (lands in M4) |
| `BTM_OVERRIDE_LIBC` | `ON` | Compile in libc symbol aliases for `LD_PRELOAD` (lands in M3) |
| `BTM_DEBUG_FILL` | `OFF` | Fill freed memory with `0xDF` (debug aid) |
| `BTM_ASAN` | `OFF` | Build with AddressSanitizer (forces `BTM_OVERRIDE_LIBC=OFF`) |

## Using

### Prefixed API

```c
#include <btmalloc.h>

void *p = btm_malloc(1024);
btm_free(p);
```

Link against `libbtmalloc.so` (or `libbtmalloc.a`).

### LD_PRELOAD drop-in (M3+)

```sh
LD_PRELOAD=$PWD/build/libbtmalloc.so your-program
```

## Roadmap

- **M0** — CMake skeleton, dumb mmap-per-alloc baseline. ✔ current
- **M1** — Real chunked allocator with size classes, single global arena, large→mmap.
- **M2** — Per-thread arenas + per-thread cache (tcache / magazines).
- **M3** — Lock-free remote-free, real `realloc` with `mremap`, full `LD_PRELOAD` overrides.
- **M4** — Microbenchmark harness; first glibc-vs-btmalloc baseline.
- **M5+** — Data-driven optimization (THP hints, page decommit, …).

See `docs/DESIGN.md` for the architectural notes once they land.

## License

MIT — see [LICENSE](LICENSE).
