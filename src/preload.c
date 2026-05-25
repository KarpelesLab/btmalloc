/*
 * btmalloc — libc symbol overrides for LD_PRELOAD.
 *
 * Compiled only into the shared library. When LD_PRELOAD'd, these shadow
 * glibc's malloc family.
 *
 * PC anchoring: malloc/calloc/realloc capture __builtin_return_address(0) —
 * the user's call site — and pass it straight to the RA-threaded core entry
 * points. Each wrapper is exactly one frame deep, so the captured address is
 * the real caller, not this shim. (aligned_alloc/posix_memalign go through the
 * public entry points and thus anchor on this shim; aligned small allocations
 * are rare, so that imprecision is acceptable for now.)
 *
 * glibc reaches malloc internally via the __libc_* names too (strdup,
 * vasprintf, getline, ...); aliasing those keeps all allocations flowing
 * through btmalloc so they can be freed through it.
 */

#include "internal.h"

#include <stddef.h>

#define BTM_EXPORT __attribute__((visibility("default")))

BTM_EXPORT void *malloc(size_t size) {
    return btm_malloc_at(size, __builtin_return_address(0));
}
BTM_EXPORT void free(void *ptr) { btm_free(ptr); }
BTM_EXPORT void *calloc(size_t n, size_t s) {
    return btm_calloc_at(n, s, __builtin_return_address(0));
}
BTM_EXPORT void *realloc(void *p, size_t s) {
    return btm_realloc_at(p, s, __builtin_return_address(0));
}
BTM_EXPORT void *reallocarray(void *p, size_t n, size_t s) {
    return btm_reallocarray(p, n, s);
}

BTM_EXPORT void *__libc_malloc(size_t size) {
    return btm_malloc_at(size, __builtin_return_address(0));
}
BTM_EXPORT void __libc_free(void *ptr) { btm_free(ptr); }
BTM_EXPORT void *__libc_calloc(size_t n, size_t s) {
    return btm_calloc_at(n, s, __builtin_return_address(0));
}
BTM_EXPORT void *__libc_realloc(void *p, size_t s) {
    return btm_realloc_at(p, s, __builtin_return_address(0));
}

BTM_EXPORT int posix_memalign(void **out, size_t align, size_t size) {
    return btm_posix_memalign(out, align, size);
}
BTM_EXPORT void *aligned_alloc(size_t align, size_t size) {
    return btm_aligned_alloc(align, size);
}
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

/* Tuning/introspection no-ops so programs that call them keep running. */
BTM_EXPORT int mallopt(int param, int value) { (void)param; (void)value; return 1; }
