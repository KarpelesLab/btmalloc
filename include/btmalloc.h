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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BTMALLOC_H */
