/*
 * btmalloc — internal types and declarations (Phase A: PC-anchored core).
 *
 * Private to src/. Public surface is include/btmalloc.h. Everything here is
 * hidden by -fvisibility=hidden.
 *
 * Design summary (see docs/DESIGN.md): allocations are grouped into P
 * PARTITIONS by hash(return_address). Each (partition, size_class) owns a pool
 * of SLABS carved from 2 MiB CHUNKS. A per-thread cache of bins, keyed by
 * (partition, size_class), fronts the pools for a lock-free hot path. A freed
 * pointer is resolved to its owning slab — and thus its home partition — via a
 * radix registry over 2 MiB regions, so it returns to the correct partition
 * regardless of which thread frees it.
 */

#ifndef BTM_INTERNAL_H
#define BTM_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btmalloc.h"

#define BTM_LIKELY(x)   __builtin_expect(!!(x), 1)
#define BTM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BTM_HIDDEN      __attribute__((visibility("hidden")))

/* ---- Geometry ---- */
#define BTM_PAGE_SHIFT       12
#define BTM_PAGE_SIZE        ((size_t)1 << BTM_PAGE_SHIFT)        /* 4 KiB */
#define BTM_CHUNK_SHIFT      21
#define BTM_CHUNK_SIZE       ((size_t)1 << BTM_CHUNK_SHIFT)       /* 2 MiB */
#define BTM_PAGES_PER_CHUNK  (BTM_CHUNK_SIZE >> BTM_PAGE_SHIFT)   /* 512  */

#define BTM_SMALL_MAX_SIZE   ((size_t)16384)
#define BTM_NUM_SIZE_CLASSES 36

#define BTM_PAGE_NONE        0xFFFFu  /* page_owner sentinel: page not in a slab */

#define BTM_CHUNK_MAGIC      ((uint64_t)0xB7A110C0CB7C7BEEULL)
#define BTM_LARGE_MAGIC      ((uint64_t)0xB7A110CC1A56A110ULL)

/* ---- Forward decls ---- */
struct btm_partition;
typedef struct btm_partition btm_partition_t;
struct btm_slab;
typedef struct btm_slab btm_slab_t;

struct btm_scpool;
typedef struct btm_scpool btm_scpool_t;

/* ---- Chunk: a 2 MiB, 2 MiB-aligned region hosting small-object slabs ----
 * Page 0 is the header. Pages 1..511 carry slabs. page_owner[p] gives the page
 * index of the slab header owning page p (or BTM_PAGE_NONE). Each chunk belongs
 * to exactly one (partition, size_class) pool, so a chunk drains and is
 * reclaimed as a unit when the call sites feeding that pool stop allocating. */
typedef struct btm_chunk {
    uint64_t          magic;          /* BTM_CHUNK_MAGIC */
    btm_scpool_t     *pool;           /* owning pool */
    struct btm_chunk *next, *prev;    /* pool's chunk list */
    uint32_t          next_free_page; /* bump pointer for slab carving */
    uint32_t          live_slabs;     /* carved slabs not yet fully freed */
    uint16_t          page_owner[BTM_PAGES_PER_CHUNK];
} btm_chunk_t;

_Static_assert(sizeof(btm_chunk_t) <= BTM_PAGE_SIZE,
               "chunk header must fit in page 0");

/* ---- Slab: a run of pages dedicated to one (partition, size_class) ----
 * Header is inline at the slab's first page; slots follow it. Free slots are
 * threaded into free_head (first word = next free slot). */
struct btm_slab {
    uint64_t         magic;       /* reserved; chunk magic is the gate */
    btm_partition_t *part;        /* home partition */
    void            *free_head;   /* intra-slab freelist */
    btm_slab_t      *next, *prev; /* membership in pool->partial */
    uint32_t         nslots;      /* total slots in this slab */
    uint32_t         free_count;  /* slots currently in free_head */
    uint16_t         sc;          /* size class index */
    uint16_t         npages;      /* pages spanned */
    uint16_t         part_idx;    /* owning partition index (for bin lookup) */
    uint8_t          in_partial;  /* on the pool's partial list? */
    uint8_t          decommitted; /* data pages released via MADV_DONTNEED? */
};

/* ---- Size-class pool: per (partition, size_class) ----
 * Owns its own chunks so a pool's memory can be released wholesale when its
 * call sites go quiet. `partial` holds slabs with free slots in use; `empty`
 * holds fully-free slabs kept for cheap recycling; `active` is the chunk being
 * carved. */
struct btm_scpool {
    pthread_mutex_t lock;
    btm_slab_t     *partial;     /* doubly-linked, free_count in (0, nslots) */
    btm_slab_t     *empty_warm;  /* committed empty slabs, fast recycle (->next) */
    btm_slab_t     *empty_cold;  /* decommitted empty slabs, re-fault on reuse */
    uint32_t        warm_count;  /* length of empty_warm */
    btm_chunk_t    *chunks;      /* doubly-linked list of this pool's chunks */
    btm_chunk_t    *active;      /* current carving chunk */
    uint64_t        nslabs;
};

struct btm_partition {
    btm_scpool_t pools[BTM_NUM_SIZE_CLASSES];
};

/* ---- Per-thread cache ----
 * A dense, directly-indexed array of bins: bin = bins[partition * NUM_SC + sc].
 * No hashing, no eviction — the index *is* the key. The array spans every
 * (partition, size_class), but it is mmap'd, so bins a thread never touches
 * never fault in and cost no memory. Each bin records its owning pool so a
 * flush (including the cross-thread case) can return slots without recomputing
 * anything. */
typedef struct btm_tls_bin {
    void         *free_head; /* cached freelist */
    btm_scpool_t *pool;      /* owning pool, set on first use */
    uint32_t      count;     /* slots in free_head */
    uint32_t      _pad;
} btm_tls_bin_t;

/* Per-thread region cache: maps a 2 MiB region -> its owning header, so the
 * common case of freeing into recently-touched regions skips the radix walk.
 * Validated against btm_registry_gen, which is bumped whenever a region's
 * owner is cleared (large free, or chunk reclaim in later phases), so stale
 * entries are detected with one cheap compare. */
#define BTM_RCACHE 16 /* power of two */
typedef struct btm_rcache_ent {
    uintptr_t region;
    uintptr_t owner;
    uint64_t  gen;
} btm_rcache_ent_t;

typedef struct btm_tls {
    btm_rcache_ent_t rcache[BTM_RCACHE];
    btm_tls_bin_t    bins[]; /* btm_nparts * BTM_NUM_SIZE_CLASSES entries */
} btm_tls_t;

/* ---- Globals (defined in btmalloc.c) ---- */
extern btm_partition_t *btm_partitions BTM_HIDDEN;
extern unsigned         btm_nparts BTM_HIDDEN;          /* power of two */
extern _Atomic(int)     btm_ready BTM_HIDDEN;
void                    btm_ensure_init(void) BTM_HIDDEN;

/* ---- size_class.c ---- */
#define BTM_SC_LUT_ENTRIES (BTM_SMALL_MAX_SIZE / 16) /* 1024 */
extern const uint32_t btm_sc_to_size[BTM_NUM_SIZE_CLASSES] BTM_HIDDEN;
extern const uint16_t btm_sc_run_pages[BTM_NUM_SIZE_CLASSES] BTM_HIDDEN;
extern uint8_t        btm_sc_lut[BTM_SC_LUT_ENTRIES] BTM_HIDDEN;
void btm_size_class_init(void) BTM_HIDDEN;        /* builds the lookup table */

/* Smallest size class fitting `size`, or -1 if 0 or > SMALL_MAX. Hot path. */
static inline int btm_size_to_sc(size_t size) {
    if (BTM_UNLIKELY(size == 0)) return -1;
    if (BTM_UNLIKELY(size > BTM_SMALL_MAX_SIZE)) return -1;
    return btm_sc_lut[(size - 1) >> 4];
}

/* Per-class TLS cache cap: ~16 KiB cached per bin, clamped to [8, 256]. */
static inline unsigned btm_cache_max(int sc) {
    unsigned m = (unsigned)(16384u / btm_sc_to_size[sc]);
    if (m < 8) m = 8;
    if (m > 256) m = 256;
    return m;
}

/* ---- chunk.c: pointer registry ---- */
extern _Atomic(uint64_t) btm_registry_gen BTM_HIDDEN; /* bumped on any removal */
void        btm_chunk_init(void) BTM_HIDDEN;
/* Resolve a user pointer to its owning header base, or 0 if foreign.
 * The returned address begins with a magic word (CHUNK or LARGE). */
uintptr_t   btm_registry_lookup(const void *ptr) BTM_HIDDEN;

/* ---- background.c: warm-chunk pool + async backing store (Phase C) ---- */
/* Obtain a 2 MiB chunk for `pool` (from the warm pool or a fresh mmap),
 * header initialized and ready to carve. */
btm_chunk_t *btm_chunk_obtain(btm_scpool_t *pool) BTM_HIDDEN;
/* Release a fully-drained chunk: queued for async MADV_DONTNEED and returned
 * to the warm pool (or trimmed to the OS), off the calling thread. */
void        btm_chunk_dispose(btm_chunk_t *c) BTM_HIDDEN;
/* Reset async/background state in a fork() child (no maintenance thread there). */
void        btm_bg_atfork_child(void) BTM_HIDDEN;
/* Register / unregister the 2 MiB regions [base, base+len) -> owner. */
void        btm_registry_insert(uintptr_t base, size_t len, uintptr_t owner) BTM_HIDDEN;
void        btm_registry_remove(uintptr_t base, size_t len) BTM_HIDDEN;
/* Resolve a pointer to its owning header base, using the per-thread region
 * cache first and falling back to the radix registry. Returns 0 if foreign. */
static inline uintptr_t btm_resolve_owner(btm_tls_t *t, const void *ptr) {
    if (BTM_UNLIKELY(t == NULL)) return btm_registry_lookup(ptr);
    uintptr_t region = (uintptr_t)ptr >> BTM_CHUNK_SHIFT;
    uint64_t gen = atomic_load_explicit(&btm_registry_gen, memory_order_acquire);
    unsigned i = (unsigned)region & (BTM_RCACHE - 1);
    btm_rcache_ent_t *e = &t->rcache[i];
    if (BTM_LIKELY(e->region == region && e->gen == gen && e->owner))
        return e->owner;
    uintptr_t owner = btm_registry_lookup(ptr);
    if (owner) { e->region = region; e->owner = owner; e->gen = gen; }
    return owner;
}

/* Given a pointer known to live in chunk `c`, return its slab. */
static inline btm_slab_t *btm_slab_of(btm_chunk_t *c, const void *ptr) {
    uint32_t page = (uint32_t)(((uintptr_t)ptr - (uintptr_t)c) >> BTM_PAGE_SHIFT);
    uint16_t owner = c->page_owner[page];
    return (btm_slab_t *)((char *)c + (size_t)owner * BTM_PAGE_SIZE);
}

/* ---- partition.c: slab pools, carving, refill/flush, reclaim ---- */
/* Carve or recycle a slab for (part, sc). Caller holds pool->lock. */
btm_slab_t *btm_slab_new(btm_partition_t *part, int sc) BTM_HIDDEN;
/* Refill `bin` from (part, sc): returns one slot, caches up to `want`-1 more. */
void *btm_pool_refill(btm_partition_t *part, int sc, btm_tls_bin_t *bin,
                      unsigned want) BTM_HIDDEN;
/* Return slots from `bin` to their home pool until count <= keep. */
void  btm_pool_flush(btm_tls_bin_t *bin, unsigned keep) BTM_HIDDEN;
/* Return a single slot directly to its slab (fallback when no TLS cache). */
void  btm_pool_free_one(btm_slab_t *slab, void *ptr) BTM_HIDDEN;

/* ---- tcache.c ---- */
extern _Thread_local btm_tls_t *btm_tls;          /* NULL until first use */
btm_tls_t     *btm_tls_get(void) BTM_HIDDEN;      /* lazily create per-thread */

/* Dense bin lookup: the index is the key, so this is a pointer add. */
static inline btm_tls_bin_t *btm_tls_bin_at(btm_tls_t *t, unsigned part,
                                            int sc) {
    return &t->bins[part * BTM_NUM_SIZE_CLASSES + (unsigned)sc];
}

/* ---- large.c ---- */
void  *btm_large_alloc(size_t size, size_t alignment) BTM_HIDDEN;
void   btm_large_free(void *base) BTM_HIDDEN;     /* base = registry owner */
size_t btm_large_usable_size(uintptr_t base, const void *ptr) BTM_HIDDEN;
void  *btm_large_realloc(uintptr_t base, void *ptr, size_t newsz) BTM_HIDDEN;

/* ---- btmalloc.c: internal entry points (RA-threaded) ---- */
void *btm_malloc_at(size_t size, void *ra) BTM_HIDDEN;
void *btm_calloc_at(size_t n, size_t size, void *ra) BTM_HIDDEN;
void *btm_realloc_at(void *ptr, size_t size, void *ra) BTM_HIDDEN;

/* Partition selector: hash the return address. */
static inline unsigned btm_partition_of(void *ra) {
    uintptr_t x = (uintptr_t)ra;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 29;
    return (unsigned)x & (btm_nparts - 1);
}

#endif /* BTM_INTERNAL_H */
