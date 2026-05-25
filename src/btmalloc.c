/*
 * btmalloc — M0 baseline allocator.
 *
 * Strategy: every allocation is a separate mmap. We prepend a 16-byte
 * header that records the mmap base address and the mmap length so that
 * btm_free can recover them and call munmap. For non-default alignments
 * we oversize the mmap and search for an aligned slot inside it.
 *
 * This is deliberately the dumbest correct implementation. It exists to
 * prove the build + test + LD_PRELOAD pipeline. M1 replaces this with a
 * real chunked allocator.
 */

#include "btmalloc.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Header laid out immediately before the user-visible pointer.
 *
 *   user_ptr - 16 : mmap_base  (uintptr_t)
 *   user_ptr -  8 : mmap_len   (uintptr_t)
 *
 * Both fields are needed because the user pointer is not necessarily
 * adjacent to the mmap base (oversized mappings for high alignment).
 */
#define BTM_HEADER_SIZE 16

/* Largest power-of-two alignment we satisfy without oversize-search.
 * Anything below this is satisfied by mmap's page alignment plus the
 * 16-byte header offset trick. */
#define BTM_FAST_ALIGN BTM_ALIGNMENT

/* Lazily-cached page size. mmap's MAP_ANONYMOUS already guarantees page
 * alignment of the returned base; we only need the page size to round
 * mapping lengths up. */
static size_t btm_page_size(void) {
    static size_t cached;
    size_t v = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (v == 0) {
        long p = sysconf(_SC_PAGESIZE);
        v = (p > 0) ? (size_t)p : 4096u;
        __atomic_store_n(&cached, v, __ATOMIC_RELAXED);
    }
    return v;
}

static inline int is_pow2(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

static inline size_t round_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

/* Core allocator. Returns a pointer aligned to max(alignment, BTM_ALIGNMENT)
 * with the 16-byte header populated. Returns NULL on failure.
 *
 * Caller is responsible for: validating alignment is a power of two,
 * checking size != 0 (when desired), and computing nmemb*size safely. */
static void *btm_alloc_aligned(size_t alignment, size_t size) {
    if (alignment < BTM_ALIGNMENT) alignment = BTM_ALIGNMENT;

    /* Worst-case bytes we need: header + size, plus up to (alignment - 1)
     * slack to hit an aligned slot inside the mapping. We then round up
     * to a whole page so munmap is well-defined. */
    size_t page = btm_page_size();
    size_t slack = (alignment > BTM_ALIGNMENT) ? (alignment - 1) : 0;
    if (size > SIZE_MAX - BTM_HEADER_SIZE - slack - page) return NULL;
    size_t total = round_up(BTM_HEADER_SIZE + slack + size, page);

    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED) return NULL;

    /* Place the user pointer at the lowest address that is alignment-aligned
     * and leaves at least 16 bytes of header space behind it. */
    uintptr_t lo = (uintptr_t)base + BTM_HEADER_SIZE;
    uintptr_t user = (lo + (alignment - 1)) & ~((uintptr_t)alignment - 1);

    uintptr_t *hdr = (uintptr_t *)user;
    hdr[-2] = (uintptr_t)base;
    hdr[-1] = (uintptr_t)total;
    return (void *)user;
}

/* Recover (base, length) from the header and unmap. */
static void btm_free_internal(void *ptr) {
    uintptr_t *hdr = (uintptr_t *)ptr;
    void *base = (void *)hdr[-2];
    size_t total = (size_t)hdr[-1];
    munmap(base, total);
}

/* -------------------- public API -------------------- */

void *btm_malloc(size_t size) {
    if (size == 0) return NULL;
    return btm_alloc_aligned(BTM_ALIGNMENT, size);
}

void btm_free(void *ptr) {
    if (!ptr) return;
    btm_free_internal(ptr);
}

void *btm_calloc(size_t nmemb, size_t size) {
    size_t bytes;
    if (__builtin_mul_overflow(nmemb, size, &bytes)) return NULL;
    if (bytes == 0) return NULL;
    /* MAP_ANONYMOUS already zero-fills, so the allocation is implicitly zero. */
    return btm_alloc_aligned(BTM_ALIGNMENT, bytes);
}

/* Returns the number of payload bytes available at ptr. For M0, that's the
 * mmap length minus the offset of the user pointer inside the mapping. */
size_t btm_malloc_usable_size(const void *ptr) {
    if (!ptr) return 0;
    const uintptr_t *hdr = (const uintptr_t *)ptr;
    uintptr_t base = hdr[-2];
    size_t total = (size_t)hdr[-1];
    size_t prefix = (uintptr_t)ptr - base;
    return total - prefix;
}

void *btm_realloc(void *ptr, size_t size) {
    if (ptr == NULL) return btm_malloc(size);
    if (size == 0) { btm_free(ptr); return NULL; }

    size_t old_usable = btm_malloc_usable_size(ptr);
    /* If the new request fits inside the existing usable bytes, we can keep
     * the same allocation. This trivially handles same-size and shrink. */
    if (size <= old_usable) return ptr;

    void *new_ptr = btm_malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_usable);
    btm_free(ptr);
    return new_ptr;
}

void *btm_reallocarray(void *ptr, size_t nmemb, size_t size) {
    size_t bytes;
    if (__builtin_mul_overflow(nmemb, size, &bytes)) { errno = ENOMEM; return NULL; }
    return btm_realloc(ptr, bytes);
}

int btm_posix_memalign(void **out, size_t alignment, size_t size) {
    if (out == NULL) return EINVAL;
    if (!is_pow2(alignment) || (alignment % sizeof(void *)) != 0) return EINVAL;
    if (size == 0) { *out = NULL; return 0; }

    void *p = btm_alloc_aligned(alignment, size);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

void *btm_aligned_alloc(size_t alignment, size_t size) {
    /* C11: alignment must be a power of two and size a multiple of alignment.
     * glibc enforces both since 2.32; we match. */
    if (!is_pow2(alignment)) { errno = EINVAL; return NULL; }
    if (size == 0) return NULL;
    if (alignment != 0 && (size % alignment) != 0) { errno = EINVAL; return NULL; }
    return btm_alloc_aligned(alignment, size);
}
