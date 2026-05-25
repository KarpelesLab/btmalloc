/*
 * btmalloc — public C API.
 *
 * A malloc/realloc/free replacement library. Every exported symbol is
 * prefixed with `btm_`. When the library is built with BTM_OVERRIDE_LIBC=ON
 * the same implementations are also exposed under the libc names
 * (malloc, free, calloc, realloc, ...) so the library can be used via
 * LD_PRELOAD.
 *
 * All sizes are in bytes. Returned pointers are aligned to at least
 * `BTM_ALIGNMENT` (16 on x86_64), matching glibc's guarantee.
 */

#ifndef BTMALLOC_H
#define BTMALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library version. Bumped at each milestone tag. */
#define BTM_VERSION_MAJOR 0
#define BTM_VERSION_MINOR 0
#define BTM_VERSION_PATCH 1

/* Minimum alignment of any pointer returned by btm_malloc/btm_calloc/btm_realloc. */
#define BTM_ALIGNMENT 16

#if defined(__GNUC__) || defined(__clang__)
#define BTM_API __attribute__((visibility("default")))
#define BTM_MALLOC_LIKE __attribute__((malloc))
#define BTM_ALLOC_SIZE(...) __attribute__((alloc_size(__VA_ARGS__)))
#define BTM_ALLOC_ALIGN(idx) __attribute__((alloc_align(idx)))
#else
#define BTM_API
#define BTM_MALLOC_LIKE
#define BTM_ALLOC_SIZE(...)
#define BTM_ALLOC_ALIGN(idx)
#endif

/* Allocate `size` bytes. Returns NULL on failure or if size is 0. */
BTM_API BTM_MALLOC_LIKE BTM_ALLOC_SIZE(1)
void *btm_malloc(size_t size);

/* Free a pointer returned by btm_malloc / btm_calloc / btm_realloc /
 * btm_aligned_alloc / btm_posix_memalign. Passing NULL is a no-op. */
BTM_API
void btm_free(void *ptr);

/* Allocate nmemb * size bytes, zeroed. Returns NULL on overflow or failure. */
BTM_API BTM_MALLOC_LIKE BTM_ALLOC_SIZE(1, 2)
void *btm_calloc(size_t nmemb, size_t size);

/* Resize allocation.
 *   btm_realloc(NULL, n)  is equivalent to btm_malloc(n).
 *   btm_realloc(p, 0)     frees p and returns NULL (matches glibc).
 * On failure, returns NULL and leaves `ptr` valid and unchanged. */
BTM_API BTM_ALLOC_SIZE(2)
void *btm_realloc(void *ptr, size_t size);

/* Same as realloc but with overflow-checked size = nmemb * size. */
BTM_API BTM_ALLOC_SIZE(2, 3)
void *btm_reallocarray(void *ptr, size_t nmemb, size_t size);

/* POSIX-style aligned allocation. `alignment` must be a power of two and
 * a multiple of sizeof(void *). Returns 0 on success, EINVAL or ENOMEM
 * on failure. */
BTM_API
int btm_posix_memalign(void **out, size_t alignment, size_t size);

/* C11-style aligned allocation. `alignment` must be a power of two; `size`
 * must be a multiple of `alignment` (glibc enforces this since 2.32 — we
 * match). Returns NULL on failure. */
BTM_API BTM_MALLOC_LIKE BTM_ALLOC_SIZE(2) BTM_ALLOC_ALIGN(1)
void *btm_aligned_alloc(size_t alignment, size_t size);

/* Returns the number of usable bytes at `ptr` (may exceed the size that was
 * requested when ptr was allocated). Returns 0 for NULL. */
BTM_API
size_t btm_malloc_usable_size(const void *ptr);

/* Writes a heap profile to file descriptor `fd`: one line per partition that
 * holds memory, with the bytes outstanding and a representative call site
 * (symbolized via dladdr when possible). Because btmalloc groups allocations
 * by call site, this attributes live memory to where it was allocated — a
 * zero-instrumentation heap profiler. Safe to call at any time. */
BTM_API
void btm_heap_profile(int fd);

/* Compact the heap by meshing: sparse slabs of the same size class whose live
 * slots do not overlap are consolidated onto shared physical pages (the others
 * are released to the OS), without moving objects in the virtual address space
 * (pointers stay valid). Returns the number of bytes of physical memory
 * reclaimed.
 *
 * Only does anything when the allocator was started in mesh mode (BTM_MESH=1),
 * which is backed by a memfd. THE CALLER MUST ENSURE THE HEAP IS QUIESCENT —
 * no other thread may allocate, free, or access heap objects during the call —
 * because live objects are physically relocated. Trivially safe for a
 * single-threaded program; a threaded program must pause its workers first.
 * Returns 0 if not in mesh mode. */
BTM_API
size_t btm_compact(void);

/* Cold-data tiering: evict the pages of "settled" allocations — slabs that are
 * full and have seen no allocation/free activity since the previous call — to
 * swap (MADV_PAGEOUT), reducing the resident footprint of long-lived but cold
 * data. The objects stay valid and fault back transparently on next access (at
 * the cost of a page fault + swap-in). Returns the number of bytes hinted for
 * eviction. Intended to be called when entering an idle period; safe to call
 * any time (mislabeled-hot pages simply fault back). Requires swap to actually
 * reduce RSS. Works in any mode. */
BTM_API
size_t btm_pageout_cold(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BTMALLOC_H */
