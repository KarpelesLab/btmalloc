/*
 * partition.c — per-(partition, size_class) slab pools: carving, recycling,
 * refill/flush, and reclaim.
 *
 * Phase B adds lifetime cohorting: because call-site-grouped objects tend to
 * die together, a pool's slabs drain in bursts. When a slab's free_count
 * reaches nslots, no slot of it is outstanding anywhere (cached slots are not
 * counted in free_count), so it is safe to reclaim under the pool lock. When
 * an entire chunk drains, its pages are returned to the OS — the active chunk
 * via MADV_DONTNEED+reset (no map/unmap thrash), older chunks via munmap.
 *
 * Lock order: pool->lock is held across carving and reclaim; chunk map/unmap
 * take registry_lock internally. No cycles.
 */

#include "internal.h"

#include <string.h>
#include <sys/mman.h>

/* Keep a few committed empty slabs per pool for cheap recycling; decommit the
 * rest (return their pages to the OS) to fight fragmentation pinning. */
#define BTM_KEEP_WARM_EMPTY 2

static inline btm_chunk_t *chunk_of(const void *p) {
    return (btm_chunk_t *)((uintptr_t)p & ~(BTM_CHUNK_SIZE - 1));
}

/* Release an empty slab's data pages (everything past its inline header page).
 * Only possible for multi-page slabs; 1-page slabs share their page with the
 * header and must wait for whole-chunk reclaim. */
static void slab_decommit(btm_slab_t *slab) {
    if (slab->npages >= 2 && !slab->decommitted) {
        madvise((char *)slab + BTM_PAGE_SIZE,
                (size_t)(slab->npages - 1) * BTM_PAGE_SIZE, MADV_DONTNEED);
        slab->decommitted = 1;
    }
}

/* ---- partial list (doubly-linked via next/prev) ---- */

static inline void partial_push(btm_scpool_t *pool, btm_slab_t *slab) {
    slab->prev = NULL;
    slab->next = pool->partial;
    if (pool->partial) pool->partial->prev = slab;
    pool->partial = slab;
    slab->in_partial = 1;
}

static inline void partial_remove(btm_scpool_t *pool, btm_slab_t *slab) {
    if (slab->prev) slab->prev->next = slab->next;
    else pool->partial = slab->next;
    if (slab->next) slab->next->prev = slab->prev;
    slab->next = slab->prev = NULL;
    slab->in_partial = 0;
}

/* ---- slab formatting ---- */

static void slab_format(btm_slab_t *slab, btm_partition_t *part, int sc,
                        unsigned npages) {
    size_t slot = btm_sc_to_size[sc];
    size_t hdr = (sizeof(btm_slab_t) + 15) & ~(size_t)15;
    char  *data = (char *)slab + hdr;
    size_t avail = (size_t)npages * BTM_PAGE_SIZE - hdr;
    uint32_t nslots = (uint32_t)(avail / slot);

    slab->magic = 0;
    slab->part = part;
    slab->part_idx = (uint16_t)(part - btm_partitions);
    slab->sc = (uint16_t)sc;
    slab->npages = (uint16_t)npages;
    slab->nslots = nslots;
    slab->free_count = nslots;
    slab->next = slab->prev = NULL;
    slab->in_partial = 0;

    void *next = NULL;
    for (uint32_t i = nslots; i-- > 0;) {
        void *s = data + (size_t)i * slot;
        *(void **)s = next;
        next = s;
    }
    slab->free_head = next;
}

/* ---- slab allocation: recycle an empty slab or carve a new one ---- */

btm_slab_t *btm_slab_new(btm_partition_t *part, int sc) {
    btm_scpool_t *pool = &part->pools[sc];

    /* Reuse a fully-free slab if one is cached. Prefer warm (pages still
     * committed); fall back to cold (decommitted — slab_format re-touches the
     * pages, faulting them back in). */
    btm_slab_t *s = NULL;
    if (pool->empty_warm) {
        s = pool->empty_warm;
        pool->empty_warm = s->next;
        pool->warm_count--;
    } else if (pool->empty_cold) {
        s = pool->empty_cold;
        pool->empty_cold = s->next;
    }
    if (s) {
        s->decommitted = 0;
        slab_format(s, part, sc, s->npages);
        chunk_of(s)->live_slabs++;
        return s;
    }

    unsigned npages = btm_sc_run_pages[sc];
    if (!pool->active ||
        pool->active->next_free_page + npages > BTM_PAGES_PER_CHUNK) {
        btm_chunk_t *c = btm_chunk_obtain(pool);
        if (!c) return NULL;
        c->next = pool->chunks;
        c->prev = NULL;
        if (pool->chunks) pool->chunks->prev = c;
        pool->chunks = c;
        pool->active = c;
    }

    btm_chunk_t *c = pool->active;
    uint32_t page = c->next_free_page;
    c->next_free_page += npages;
    for (uint32_t p = page; p < page + npages; p++)
        c->page_owner[p] = (uint16_t)page;
    c->live_slabs++;
    pool->nslabs++;

    btm_slab_t *slab = (btm_slab_t *)((char *)c + (size_t)page * BTM_PAGE_SIZE);
    slab_format(slab, part, sc, npages);
    return slab;
}

/* ---- reclaim ---- */

static void chunk_unlink(btm_scpool_t *pool, btm_chunk_t *c) {
    if (c->prev) c->prev->next = c->next;
    else pool->chunks = c->next;
    if (c->next) c->next->prev = c->prev;
    c->next = c->prev = NULL;
}

/* Drop every cached empty slab that lives in chunk `c` from both empty lists. */
static void purge_empties_of_chunk(btm_scpool_t *pool, btm_chunk_t *c) {
    btm_slab_t **pp = &pool->empty_warm;
    while (*pp) {
        if (chunk_of(*pp) == c) { *pp = (*pp)->next; pool->warm_count--; }
        else pp = &(*pp)->next;
    }
    pp = &pool->empty_cold;
    while (*pp) {
        if (chunk_of(*pp) == c) *pp = (*pp)->next;
        else pp = &(*pp)->next;
    }
}

/* Called when `slab` has just become fully free. Caller holds pool->lock. */
static void slab_became_free(btm_scpool_t *pool, btm_slab_t *slab) {
    partial_remove(pool, slab);
    btm_chunk_t *c = chunk_of(slab);
    c->live_slabs--;

    if (c->live_slabs == 0) {
        /* Whole chunk is free. Its other slabs (if any) sit in pool->empty.
         * Hand it to the warm-chunk pool: pages get released asynchronously and
         * the chunk is recycled for the next carve (here or in another pool). */
        purge_empties_of_chunk(pool, c);
        if (c == pool->active) pool->active = NULL;
        chunk_unlink(pool, c);
        btm_chunk_dispose(c);
    } else if (pool->warm_count < BTM_KEEP_WARM_EMPTY) {
        /* Keep a few empties committed for cheap recycling. */
        slab->next = pool->empty_warm;
        pool->empty_warm = slab;
        pool->warm_count++;
    } else {
        /* Beyond the warm budget: release the slab's pages to the OS and park
         * it on the cold list. This is what drains a fragmented chunk's RSS
         * without waiting for every survivor to die. */
        slab_decommit(slab);
        slab->next = pool->empty_cold;
        pool->empty_cold = slab;
    }
}

/* ---- refill / flush ---- */

void *btm_pool_refill(btm_partition_t *part, int sc, btm_tls_bin_t *bin,
                      unsigned want) {
    btm_scpool_t *pool = &part->pools[sc];
    pthread_mutex_lock(&pool->lock);

    btm_slab_t *slab = pool->partial;
    if (!slab) {
        slab = btm_slab_new(part, sc);
        if (!slab) { pthread_mutex_unlock(&pool->lock); return NULL; }
        partial_push(pool, slab);
    }

    void *local = NULL;
    unsigned n = 0;
    while (n < want && slab->free_head) {
        void *s = slab->free_head;
        slab->free_head = *(void **)s;
        *(void **)s = local;
        local = s;
        n++;
    }
    slab->free_count -= n;
    if (slab->free_count == 0) partial_remove(pool, slab);
    pthread_mutex_unlock(&pool->lock);

    if (n == 0) return NULL;

    void *p = local;
    local = *(void **)local;
    n--;
    if (n) {
        void *tail = local;
        for (unsigned i = 1; i < n; i++) tail = *(void **)tail;
        *(void **)tail = bin->free_head;
        bin->free_head = local;
        bin->count += n;
    }
    return p;
}

void btm_pool_flush(btm_tls_bin_t *bin, unsigned keep) {
    if (bin->count <= keep) return;

    btm_scpool_t *pool = bin->pool;
    pthread_mutex_lock(&pool->lock);
    while (bin->count > keep) {
        void *s = bin->free_head;
        bin->free_head = *(void **)s;
        bin->count--;

        btm_chunk_t *c = chunk_of(s);
        btm_slab_t *slab = btm_slab_of(c, s);
        *(void **)s = slab->free_head;
        slab->free_head = s;
        slab->free_count++;
        if (slab->free_count == slab->nslots) {
            slab_became_free(pool, slab);
        } else if (!slab->in_partial) {
            partial_push(pool, slab);
        }
    }
    pthread_mutex_unlock(&pool->lock);
}

void btm_pool_free_one(btm_slab_t *slab, void *ptr) {
    btm_scpool_t *pool = &slab->part->pools[slab->sc];
    pthread_mutex_lock(&pool->lock);
    *(void **)ptr = slab->free_head;
    slab->free_head = ptr;
    slab->free_count++;
    if (slab->free_count == slab->nslots) {
        slab_became_free(pool, slab);
    } else if (!slab->in_partial) {
        partial_push(pool, slab);
    }
    pthread_mutex_unlock(&pool->lock);
}
