/*
 * partition.c — per-(partition, size_class) slab pools.
 *
 * The per-thread cache (tcache.c) fronts these pools; this file owns the
 * locked slow path: pulling a batch of slots out of a slab to refill a cache
 * bin, and returning slots from a bin back to their home slabs on flush.
 *
 * Lock order: pool->lock may be held while taking chunk_lock (inside
 * btm_slab_new) and registry_lock; never the reverse. No cycles.
 */

#include "internal.h"

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

void *btm_pool_refill(btm_partition_t *part, int sc, btm_tls_bin_t *bin,
                      unsigned want) {
    btm_scpool_t *pool = &part->pools[sc];
    pthread_mutex_lock(&pool->lock);

    btm_slab_t *slab = pool->partial;
    if (!slab) {
        slab = btm_slab_new(part, sc); /* takes chunk_lock; pool->chunk order */
        if (!slab) { pthread_mutex_unlock(&pool->lock); return NULL; }
        partial_push(pool, slab);
        pool->nslabs++;
    }

    /* Detach up to `want` slots from the slab into a local chain. */
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

    /* Hand back one slot; splice the remaining (n-1) into the cache bin. */
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

    /* All slots in this bin share (partition, size_class) — recover the pool
     * from the bin key (set when the bin was claimed). */
    unsigned k = bin->key - 1;
    unsigned pidx = k / BTM_NUM_SIZE_CLASSES;
    unsigned sc = k % BTM_NUM_SIZE_CLASSES;
    btm_scpool_t *pool = &btm_partitions[pidx].pools[sc];

    pthread_mutex_lock(&pool->lock);
    while (bin->count > keep) {
        void *s = bin->free_head;
        bin->free_head = *(void **)s;
        bin->count--;

        btm_chunk_t *c = (btm_chunk_t *)((uintptr_t)s & ~(BTM_CHUNK_SIZE - 1));
        btm_slab_t *slab = btm_slab_of(c, s);
        *(void **)s = slab->free_head;
        slab->free_head = s;
        slab->free_count++;
        if (!slab->in_partial) partial_push(pool, slab);
        /* Phase B will reclaim a slab here once free_count == nslots. */
    }
    pthread_mutex_unlock(&pool->lock);
}

void btm_pool_free_one(btm_slab_t *slab, void *ptr) {
    btm_scpool_t *pool = &slab->part->pools[slab->sc];
    pthread_mutex_lock(&pool->lock);
    *(void **)ptr = slab->free_head;
    slab->free_head = ptr;
    slab->free_count++;
    if (!slab->in_partial) partial_push(pool, slab);
    pthread_mutex_unlock(&pool->lock);
}
