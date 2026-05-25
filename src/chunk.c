/*
 * chunk.c — backing store: 2 MiB chunks, slab carving, and the pointer
 * registry that maps any address back to its owning btmalloc object.
 *
 * The registry is a two-level radix tree keyed by the 2 MiB region index
 * (addr >> 21). It lets btm_free resolve a pointer WITHOUT dereferencing a
 * possibly-unmapped address: a region absent from the registry is foreign.
 * Both small chunks and large allocations register their regions (a large
 * object > 2 MiB registers all the regions it spans, each pointing at the
 * object's base header), so resolution is uniform and correct.
 */

#include "internal.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------------- radix registry ---------------- */

#define BTM_RADIX_L1_BITS 14
#define BTM_RADIX_L2_BITS 13
#define BTM_RADIX_L1_SIZE (1u << BTM_RADIX_L1_BITS)
#define BTM_RADIX_L2_SIZE (1u << BTM_RADIX_L2_BITS)

/* L1: array of pointers to lazily-mapped leaf arrays of atomic owners. */
static _Atomic(_Atomic(uintptr_t) *) radix_l1[BTM_RADIX_L1_SIZE];
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

/* Bumped whenever a region's owner is cleared, so per-thread region caches can
 * detect staleness with one compare. Inserts to fresh regions don't bump it. */
_Atomic(uint64_t) btm_registry_gen;

static inline unsigned radix_i1(uintptr_t key) {
    return (unsigned)(key >> BTM_RADIX_L2_BITS) & (BTM_RADIX_L1_SIZE - 1);
}
static inline unsigned radix_i2(uintptr_t key) {
    return (unsigned)key & (BTM_RADIX_L2_SIZE - 1);
}

uintptr_t btm_registry_lookup(const void *ptr) {
    uintptr_t key = (uintptr_t)ptr >> BTM_CHUNK_SHIFT;
    _Atomic(uintptr_t) *leaf =
        atomic_load_explicit(&radix_l1[radix_i1(key)], memory_order_acquire);
    if (!leaf) return 0;
    return atomic_load_explicit(&leaf[radix_i2(key)], memory_order_acquire);
}

/* Set one region's owner. Must be called with registry_lock held. */
static void registry_set_locked(uintptr_t key, uintptr_t owner) {
    unsigned i1 = radix_i1(key);
    _Atomic(uintptr_t) *leaf =
        atomic_load_explicit(&radix_l1[i1], memory_order_acquire);
    if (!leaf) {
        size_t bytes = BTM_RADIX_L2_SIZE * sizeof(_Atomic(uintptr_t));
        leaf = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (leaf == MAP_FAILED) return; /* lookups will treat as foreign */
        atomic_store_explicit(&radix_l1[i1], leaf, memory_order_release);
    }
    atomic_store_explicit(&leaf[radix_i2(key)], owner, memory_order_release);
}

void btm_registry_insert(uintptr_t base, size_t len, uintptr_t owner) {
    uintptr_t first = base >> BTM_CHUNK_SHIFT;
    uintptr_t last = (base + len - 1) >> BTM_CHUNK_SHIFT;
    pthread_mutex_lock(&registry_lock);
    for (uintptr_t k = first; k <= last; k++) registry_set_locked(k, owner);
    pthread_mutex_unlock(&registry_lock);
}

void btm_registry_remove(uintptr_t base, size_t len) {
    uintptr_t first = base >> BTM_CHUNK_SHIFT;
    uintptr_t last = (base + len - 1) >> BTM_CHUNK_SHIFT;
    pthread_mutex_lock(&registry_lock);
    for (uintptr_t k = first; k <= last; k++) registry_set_locked(k, 0);
    pthread_mutex_unlock(&registry_lock);
    /* Invalidate stale region-cache entries everywhere. */
    atomic_fetch_add_explicit(&btm_registry_gen, 1, memory_order_release);
}

/* ---------------- chunk map / unmap / reset ---------------- */

void btm_chunk_init(void) { /* radix_l1 + locks are static-initialized */ }

/* First page available for slabs (page 0 is the chunk header). */
#define BTM_FIRST_DATA_PAGE 1

btm_chunk_t *btm_chunk_map(btm_scpool_t *pool) {
    size_t want = BTM_CHUNK_SIZE * 2;
    void *raw = mmap(NULL, want, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (raw == MAP_FAILED) return NULL;

    uintptr_t base = ((uintptr_t)raw + BTM_CHUNK_SIZE - 1) & ~(BTM_CHUNK_SIZE - 1);
    size_t front = base - (uintptr_t)raw;
    if (front) munmap(raw, front);
    size_t back = want - front - BTM_CHUNK_SIZE;
    if (back) munmap((void *)(base + BTM_CHUNK_SIZE), back);

    btm_chunk_t *c = (btm_chunk_t *)base;
    c->magic = BTM_CHUNK_MAGIC;
    c->pool = pool;
    c->next = c->prev = NULL;
    c->next_free_page = BTM_FIRST_DATA_PAGE;
    c->live_slabs = 0;
    for (unsigned i = 0; i < BTM_PAGES_PER_CHUNK; i++)
        c->page_owner[i] = BTM_PAGE_NONE;

    /* Hint the kernel to back this chunk with a 2 MiB huge page. */
    madvise((void *)base, BTM_CHUNK_SIZE, MADV_HUGEPAGE);

    btm_registry_insert(base, BTM_CHUNK_SIZE, base);
    return c;
}

void btm_chunk_unmap(btm_chunk_t *c) {
    btm_registry_remove((uintptr_t)c, BTM_CHUNK_SIZE);
    munmap(c, BTM_CHUNK_SIZE);
}

void btm_chunk_reset(btm_chunk_t *c) {
    /* Drop physical pages back to the OS (RSS falls) but keep the chunk mapped
     * and registered so it can be re-carved without another mmap/registry
     * round-trip. The header page (0) is preserved. */
    madvise((char *)c + BTM_PAGE_SIZE, BTM_CHUNK_SIZE - BTM_PAGE_SIZE,
            MADV_DONTNEED);
    c->next_free_page = BTM_FIRST_DATA_PAGE;
    c->live_slabs = 0;
    for (unsigned i = BTM_FIRST_DATA_PAGE; i < BTM_PAGES_PER_CHUNK; i++)
        c->page_owner[i] = BTM_PAGE_NONE;
}
