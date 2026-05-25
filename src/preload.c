/*
 * btmalloc — libc symbol overrides for LD_PRELOAD.
 *
 * This translation unit is compiled only into the shared library (never the
 * static library that the tests link against, to avoid hijacking libc malloc
 * inside the test process). When the resulting libbtmalloc.so is LD_PRELOAD'd,
 * these definitions shadow glibc's malloc family.
 *
 * Phase 0 note: these are thin wrappers over the public btm_* API so we can
 * benchmark btmalloc against glibc/jemalloc immediately. Phase A will thread
 * the caller's return address (__builtin_return_address(0)) through to the
 * allocator core for PC anchoring; at that point malloc() must remain exactly
 * one frame deep so the captured RA is the user's call site.
 */

#include "btmalloc.h"

#include <stddef.h>

#define BTM_EXPORT __attribute__((visibility("default")))

/* The C library also reaches malloc internally through these __libc_* names
 * (strdup, vasprintf, getline, ...). Providing them avoids subtle bugs where
 * some allocations bypass our allocator and then get freed through it. */

BTM_EXPORT void *malloc(size_t size) { return btm_malloc(size); }
BTM_EXPORT void  free(void *ptr) { btm_free(ptr); }
BTM_EXPORT void *calloc(size_t n, size_t s) { return btm_calloc(n, s); }
BTM_EXPORT void *realloc(void *p, size_t s) { return btm_realloc(p, s); }
BTM_EXPORT void *reallocarray(void *p, size_t n, size_t s) {
    return btm_reallocarray(p, n, s);
}

BTM_EXPORT void *__libc_malloc(size_t size) { return btm_malloc(size); }
BTM_EXPORT void  __libc_free(void *ptr) { btm_free(ptr); }
BTM_EXPORT void *__libc_calloc(size_t n, size_t s) { return btm_calloc(n, s); }
BTM_EXPORT void *__libc_realloc(void *p, size_t s) { return btm_realloc(p, s); }

BTM_EXPORT int posix_memalign(void **out, size_t align, size_t size) {
    return btm_posix_memalign(out, align, size);
}
BTM_EXPORT void *aligned_alloc(size_t align, size_t size) {
    return btm_aligned_alloc(align, size);
}
/* Legacy aligned-allocation entry points. */
BTM_EXPORT void *memalign(size_t align, size_t size) {
    return btm_aligned_alloc(align, size);
}
BTM_EXPORT void *valloc(size_t size) {
    return btm_aligned_alloc(4096, (size + 4095) & ~(size_t)4095);
}
BTM_EXPORT void *pvalloc(size_t size) {
    size_t r = (size + 4095) & ~(size_t)4095;
    return btm_aligned_alloc(4096, r);
}

BTM_EXPORT size_t malloc_usable_size(void *ptr) {
    return btm_malloc_usable_size(ptr);
}

/* Weak no-op stubs for tuning/introspection calls some programs make. We do
 * not honor them yet; returning benign values keeps such programs running. */
BTM_EXPORT int mallopt(int param, int value) { (void)param; (void)value; return 1; }
