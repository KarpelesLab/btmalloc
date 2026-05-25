/*
 * large.c — allocations larger than BTM_SMALL_MAX_SIZE.
 *
 * Each large object is its own mmap, placed at a 2 MiB-aligned base so the
 * pointer registry (keyed by 2 MiB region) resolves any interior pointer back
 * to the object's header. The header sits at the base; the user pointer is a
 * fixed, alignment-satisfying offset past it. Realloc uses mremap.
 */

#include "internal.h"

#include <string.h>
#include <sys/mman.h>

typedef struct {
    uint64_t magic;     /* BTM_LARGE_MAGIC */
    size_t   map_len;   /* total bytes mapped (page-aligned) */
    size_t   user_off;  /* base -> user pointer offset */
    size_t   req_size;  /* last requested size */
} btm_large_hdr_t;

static inline size_t round_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

void *btm_large_alloc(size_t size, size_t alignment) {
    if (alignment < BTM_ALIGNMENT) alignment = BTM_ALIGNMENT;

    size_t hdr = round_up(sizeof(btm_large_hdr_t), alignment);
    size_t total = round_up(hdr + size, BTM_PAGE_SIZE);

    /* Base must be 2 MiB-aligned for the registry; if a caller demands more
     * than 2 MiB alignment, align the base to that instead. */
    size_t balign = alignment > BTM_CHUNK_SIZE ? alignment : BTM_CHUNK_SIZE;
    size_t want = total + balign;

    void *raw = mmap(NULL, want, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (raw == MAP_FAILED) return NULL;

    uintptr_t base = round_up((uintptr_t)raw, balign);
    size_t front = base - (uintptr_t)raw;
    if (front) munmap(raw, front);
    size_t back = want - front - total;
    if (back) munmap((void *)(base + total), back);

    btm_large_hdr_t *h = (btm_large_hdr_t *)base;
    h->magic = BTM_LARGE_MAGIC;
    h->map_len = total;
    h->user_off = hdr;
    h->req_size = size;

    btm_registry_insert(base, total, base);
    return (void *)(base + hdr);
}

void btm_large_free(void *base_v) {
    btm_large_hdr_t *h = base_v;
    size_t total = h->map_len;
    btm_registry_remove((uintptr_t)base_v, total);
    munmap(base_v, total);
}

size_t btm_large_usable_size(uintptr_t base, const void *ptr) {
    const btm_large_hdr_t *h = (const btm_large_hdr_t *)base;
    return h->map_len - ((uintptr_t)ptr - base);
}

void *btm_large_realloc(uintptr_t base, void *ptr, size_t newsz) {
    btm_large_hdr_t *h = (btm_large_hdr_t *)base;
    size_t user_off = h->user_off;
    size_t old_total = h->map_len;
    size_t new_total = round_up(user_off + newsz, BTM_PAGE_SIZE);

    if (new_total == old_total) {
        h->req_size = newsz;
        return ptr;
    }

    /* Drop the old registry entries, remap, re-register at the (possibly new)
     * base. mremap may move the mapping; the header travels with it. */
    btm_registry_remove(base, old_total);
    void *nb = mremap((void *)base, old_total, new_total, MREMAP_MAYMOVE);
    if (nb == MAP_FAILED) {
        btm_registry_insert(base, old_total, base); /* restore */
        return NULL;
    }

    uintptr_t newbase = (uintptr_t)nb;
    btm_large_hdr_t *nh = (btm_large_hdr_t *)newbase;
    nh->magic = BTM_LARGE_MAGIC;
    nh->map_len = new_total;
    nh->user_off = user_off;
    nh->req_size = newsz;
    btm_registry_insert(newbase, new_total, newbase);
    return (void *)(newbase + user_off);
}
