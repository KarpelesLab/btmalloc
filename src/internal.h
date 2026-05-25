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

/* ---- Chunk: a 2 MiB, 2 MiB-aligned region hosting small-object slabs ----
 * Page 0 is the header. Pages 1..511 carry slabs. page_owner[p] gives the page
 * index of the slab header owning page p (or BTM_PAGE_NONE). */
typedef struct btm_chunk {
    uint64_t          magic;          /* BTM_CHUNK_MAGIC */
    struct btm_chunk *next;           /* global chunk list */
    uint32_t          next_free_page; /* bump pointer for slab carving */
    uint32_t          _pad;
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
    uint8_t          in_partial;  /* on the pool's partial list? */
    uint8_t          _pad[3];
};

/* ---- Size-class pool: per (partition, size_class) ---- */
typedef struct btm_scpool {
    pthread_mutex_t lock;
    btm_slab_t     *partial;   /* slabs with free_count > 0 */
    uint64_t        nslabs;
} btm_scpool_t;

struct btm_partition {
    btm_scpool_t pools[BTM_NUM_SIZE_CLASSES];
};

/* ---- Per-thread cache ----
 * Direct-mapped cache of bins, each keyed by (partition, size_class). A miss
 * evicts the resident bin (flushing its slots back to their home pool) and
 * claims the slot for the new key. */
#define BTM_TLS_BINS 512  /* power of two */
typedef struct btm_tls_bin {
    uint32_t key;        /* part * NUM_SIZE_CLASSES + sc, +1 (0 = empty) */
    uint32_t count;      /* slots in free_head */
    void    *free_head;  /* cached freelist */
} btm_tls_bin_t;

typedef struct btm_tls {
    btm_tls_bin_t bins[BTM_TLS_BINS];
} btm_tls_t;

/* ---- Globals (defined in btmalloc.c) ---- */
extern btm_partition_t *btm_partitions BTM_HIDDEN;
extern unsigned         btm_nparts BTM_HIDDEN;          /* power of two */
extern _Atomic(int)     btm_ready BTM_HIDDEN;
void                    btm_ensure_init(void) BTM_HIDDEN;

/* ---- size_class.c ---- */
extern const uint32_t btm_sc_to_size[BTM_NUM_SIZE_CLASSES] BTM_HIDDEN;
extern const uint16_t btm_sc_run_pages[BTM_NUM_SIZE_CLASSES] BTM_HIDDEN;
void btm_size_class_init(void) BTM_HIDDEN;        /* builds the lookup table */
int  btm_size_to_sc(size_t size) BTM_HIDDEN;      /* -1 if > SMALL_MAX or 0 */
unsigned btm_cache_max(int sc) BTM_HIDDEN;        /* per-class TLS cache cap */

/* ---- chunk.c: backing store + pointer registry + slab carving ---- */
void        btm_chunk_init(void) BTM_HIDDEN;
/* Carve a fresh slab for (part, sc) from the global chunk pool. */
btm_slab_t *btm_slab_new(btm_partition_t *part, int sc) BTM_HIDDEN;
/* Resolve a user pointer to its owning header base, or 0 if foreign.
 * The returned address begins with a magic word (CHUNK or LARGE). */
uintptr_t   btm_registry_lookup(const void *ptr) BTM_HIDDEN;
/* Register / unregister the 2 MiB regions [base, base+len) -> owner. */
void        btm_registry_insert(uintptr_t base, size_t len, uintptr_t owner) BTM_HIDDEN;
void        btm_registry_remove(uintptr_t base, size_t len) BTM_HIDDEN;
/* Given a pointer known to live in chunk `c`, return its slab. */
static inline btm_slab_t *btm_slab_of(btm_chunk_t *c, const void *ptr) {
    uint32_t page = (uint32_t)(((uintptr_t)ptr - (uintptr_t)c) >> BTM_PAGE_SHIFT);
    uint16_t owner = c->page_owner[page];
    return (btm_slab_t *)((char *)c + (size_t)owner * BTM_PAGE_SIZE);
}

/* ---- partition.c: pool refill/flush under lock ---- */
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
btm_tls_bin_t *btm_tls_bin_for(btm_tls_t *t, unsigned key) BTM_HIDDEN;

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
