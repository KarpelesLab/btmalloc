/*
 * chunk.c — backing store: 2 MiB chunks, slab carving, and the owner table
 * that maps any address back to its owning btmalloc object.
 *
 * The owner table is keyed by the 2 MiB region index (addr >> 21) and lets
 * btm_free resolve a pointer WITHOUT dereferencing a possibly-unmapped address:
 * a region absent from the table is foreign. Both small chunks and large
 * allocations register their regions (a large object > 2 MiB registers all the
 * regions it spans, each pointing at the object's base header), so resolution
 * is uniform and correct. The table structure is a pluggable build-time engine
 * (BTM_OWNER_ENGINE): a two-level radix (registry/nocache) or a single flat
 * lazily-mapped array (flat). See internal.h and bench/RESULTS.md.
 */

#include "internal.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Serializes writers; the lookup path is lock-free in every engine. */
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

#if BTM_OWNER_ENGINE == BTM_OWNER_FLAT
/* ---------------- flat owner table ---------------- */
/* One owner word per 2 MiB region of the (≤47-bit) user address space:
 * 2^(47-21) = 2^26 entries × 8 B = 512 MiB, reserved with MAP_NORESERVE so only
 * touched region-pages ever cost physical memory (≈one 4 KiB page per 512 live
 * regions). Resolution is a single bounds-checked atomic load — no per-thread
 * cache, no generation counter. */
#define BTM_FLAT_BITS (47 - BTM_CHUNK_SHIFT)          /* 26 */
#define BTM_FLAT_SIZE ((uintptr_t)1 << BTM_FLAT_BITS)
static _Atomic(uintptr_t) *btm_owner_flat;

void btm_chunk_init(void) {
    size_t bytes = (size_t)BTM_FLAT_SIZE * sizeof(_Atomic(uintptr_t));
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    btm_owner_flat = (p == MAP_FAILED) ? NULL : p;
}

uintptr_t btm_registry_lookup(const void *ptr) {
    uintptr_t region = (uintptr_t)ptr >> BTM_CHUNK_SHIFT;
    if (BTM_UNLIKELY(region >= BTM_FLAT_SIZE || !btm_owner_flat)) return 0;
    return atomic_load_explicit(&btm_owner_flat[region], memory_order_acquire);
}

static inline void flat_set(uintptr_t region, uintptr_t owner) {
    if (BTM_UNLIKELY(region >= BTM_FLAT_SIZE || !btm_owner_flat)) return;
    atomic_store_explicit(&btm_owner_flat[region], owner, memory_order_release);
}

void btm_registry_insert(uintptr_t base, size_t len, uintptr_t owner) {
    uintptr_t first = base >> BTM_CHUNK_SHIFT;
    uintptr_t last = (base + len - 1) >> BTM_CHUNK_SHIFT;
    pthread_mutex_lock(&registry_lock);
    for (uintptr_t k = first; k <= last; k++) flat_set(k, owner);
    pthread_mutex_unlock(&registry_lock);
}

void btm_registry_remove(uintptr_t base, size_t len) {
    uintptr_t first = base >> BTM_CHUNK_SHIFT;
    uintptr_t last = (base + len - 1) >> BTM_CHUNK_SHIFT;
    pthread_mutex_lock(&registry_lock);
    for (uintptr_t k = first; k <= last; k++) flat_set(k, 0);
    pthread_mutex_unlock(&registry_lock);
}

#else
/* ---------------- radix registry (registry / nocache) ---------------- */

#define BTM_RADIX_L1_BITS 14
#define BTM_RADIX_L2_BITS 13
#define BTM_RADIX_L1_SIZE (1u << BTM_RADIX_L1_BITS)
#define BTM_RADIX_L2_SIZE (1u << BTM_RADIX_L2_BITS)

/* L1: array of pointers to lazily-mapped leaf arrays of atomic owners. */
static _Atomic(_Atomic(uintptr_t) *) radix_l1[BTM_RADIX_L1_SIZE];

#if BTM_OWNER_ENGINE == BTM_OWNER_REGISTRY
/* Bumped whenever a region's owner is cleared, so per-thread region caches can
 * detect staleness with one compare. Inserts to fresh regions don't bump it. */
_Atomic(uint64_t) btm_registry_gen;
#endif

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
#if BTM_OWNER_ENGINE == BTM_OWNER_REGISTRY
    /* Invalidate stale region-cache entries everywhere. */
    atomic_fetch_add_explicit(&btm_registry_gen, 1, memory_order_release);
#endif
}

void btm_chunk_init(void) { /* radix_l1 + locks are static-initialized */ }

#endif /* BTM_OWNER_ENGINE */
