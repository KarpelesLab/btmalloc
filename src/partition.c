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

/* Mark a slab as having had recent allocator activity (for cold tiering). If it
 * had been paged out, a touch implies its pages are faulting back in. */
static inline void slab_touch(btm_slab_t *slab) {
    slab->epoch = atomic_load_explicit(&btm_tier_epoch, memory_order_relaxed);
    slab->paged_out = 0;
}

/* Release an empty slab's data pages to the OS. With out-of-line descriptors
 * the data region is pure slot pages, so this now works for every class
 * (including single-page slabs). */
static void slab_decommit(btm_slab_t *slab) {
    if (slab->decommitted) return;
    btm_chunk_t *c = chunk_of(slab);
    uint64_t off = c->memfd_off + (uint64_t)slab->first_page * BTM_PAGE_SIZE;
    btm_release_pages(btm_slab_data(slab), off,
                      (size_t)slab->npages * BTM_PAGE_SIZE);
    slab->decommitted = 1;
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

/* Format a descriptor whose first_page and npages are already set. */
static void slab_format(btm_slab_t *slab, btm_partition_t *part, int sc) {
    size_t slot = btm_sc_to_size[sc];

    slab->part_idx = (uint16_t)(part - btm_partitions);
    slab->sc = (uint16_t)sc;
    slab->next = slab->prev = NULL;
    slab->in_partial = 0;
    slab->retired = 0;
    slab->decommitted = 0;
    slab->paged_out = 0;
    slab->epoch = atomic_load_explicit(&btm_tier_epoch, memory_order_relaxed);

    /* The data region is npages pure slot-only pages (no inline header). */
    char *data = btm_slab_data(slab);
    uint32_t nslots = (uint32_t)((size_t)slab->npages * BTM_PAGE_SIZE / slot);
    slab->nslots = nslots;
    slab->free_count = nslots;

    void *next = NULL;
    for (uint32_t i = nslots; i-- > 0;) {
        void *s = data + (size_t)i * slot;
        btm_fl_set(s, next);
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
        /* Recycled descriptor keeps its first_page/npages. */
        slab_format(s, part, sc);
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

    /* The descriptor lives in the chunk's descriptor array, indexed by the
     * slab's first data page; the data is the page run itself. */
    btm_slab_t *slab = &btm_chunk_descs(c)[page];
    slab->first_page = (uint16_t)page;
    slab->npages = (uint16_t)npages;
    slab_format(slab, part, sc);
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
        slab->free_head = btm_fl_get(s);
        btm_fl_set(s, local);
        local = s;
        n++;
    }
    slab->free_count -= n;
    pool->outstanding += n; /* these slots leave the pool (cache or in use) */
    slab_touch(slab);
    if (slab->free_count == 0) partial_remove(pool, slab);
    pthread_mutex_unlock(&pool->lock);

    if (n == 0) return NULL;

    void *p = local;
    local = btm_fl_get(local);
    n--;
    if (n) {
        void *tail = local;
        for (unsigned i = 1; i < n; i++) tail = btm_fl_get(tail);
        btm_fl_set(tail, bin->free_head);
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
        bin->free_head = btm_fl_get(s);
        bin->count--;
        pool->outstanding--; /* returning a slot to the pool */

        btm_chunk_t *c = chunk_of(s);
        btm_slab_t *slab = btm_slab_of(c, s);
        btm_fl_set(s, slab->free_head);
        slab->free_head = s;
        slab->free_count++;
        slab_touch(slab);
        if (slab->retired) {
            /* Meshed slab: inert — track frees but never reallocate/reclaim. */
        } else if (slab->free_count == slab->nslots) {
            slab_became_free(pool, slab);
        } else if (!slab->in_partial) {
            partial_push(pool, slab);
        }
    }
    pthread_mutex_unlock(&pool->lock);
}

/* ---- compaction (Mesh) ---- */

#define BTM_OCC_MAXSLOTS 1024
#define BTM_OCC_WORDS (BTM_OCC_MAXSLOTS / 64)

/* Build the occupied-slot bitmap (bit set = slot in use, i.e. not in the
 * freelist). Returns the occupied count. */
static unsigned build_occ(btm_slab_t *s, uint64_t *bm) {
    unsigned words = (s->nslots + 63) / 64;
    for (unsigned i = 0; i < words; i++) bm[i] = 0;
    for (uint32_t k = 0; k < s->nslots; k++) bm[k / 64] |= 1ULL << (k % 64);

    char *data = btm_slab_data(s);
    size_t slot = btm_sc_to_size[s->sc];
    for (void *f = s->free_head; f; f = btm_fl_get(f)) {
        size_t idx = (size_t)((char *)f - data) / slot;
        bm[idx / 64] &= ~(1ULL << (idx % 64));
    }
    return s->nslots - s->free_count;
}

/* Mesh donor D into recipient R (occupancy disjoint). Copies D's live objects
 * into R's matching free slots, rebuilds R's freelist to exclude them, remaps
 * D's data region onto R's, and retires D. Returns bytes reclaimed. */
static size_t mesh_pair(btm_scpool_t *pool, btm_slab_t *R, btm_slab_t *D,
                        const uint64_t *Rocc, const uint64_t *Docc) {
    int sc = R->sc;
    size_t slot = btm_sc_to_size[sc];
    char *Rd = btm_slab_data(R), *Dd = btm_slab_data(D);

    /* Copy D's live objects into R's (free) slots at the same index. */
    for (uint32_t k = 0; k < D->nslots; k++) {
        if (Docc[k / 64] & (1ULL << (k % 64)))
            memcpy(Rd + (size_t)k * slot, Dd + (size_t)k * slot, slot);
    }

    /* Rebuild R's freelist: keep slots that are free in R AND not now holding a
     * donor object. */
    void *head = NULL;
    unsigned cnt = 0;
    for (uint32_t k = R->nslots; k-- > 0;) {
        int r_occ = (Rocc[k / 64] >> (k % 64)) & 1;
        int d_occ = (Docc[k / 64] >> (k % 64)) & 1;
        if (!r_occ && !d_occ) {
            void *s = Rd + (size_t)k * slot;
            btm_fl_set(s, head);
            head = s;
            cnt++;
        }
    }
    R->free_head = head;
    R->free_count = cnt;

    size_t reclaimed = btm_mesh_remap(D, R);

    /* Retire the donor: its objects now live in R; its slots must never be
     * reallocated. R stays active for its remaining (non-donor) free slots. */
    partial_remove(pool, D);
    D->retired = 1;
    return reclaimed;
}

size_t btm_compact(void) {
    if (!btm_mesh_mode) return 0;
    if (!atomic_load_explicit(&btm_ready, memory_order_acquire)) return 0;

    size_t reclaimed = 0;
    for (unsigned p = 0; p < btm_nparts; p++) {
        for (int sc = 0; sc < BTM_NUM_SIZE_CLASSES; sc++) {
            if (btm_sc_run_pages[sc] < 2) continue; /* meshing not worth it */
            btm_scpool_t *pool = &btm_partitions[p].pools[sc];
            pthread_mutex_lock(&pool->lock);

            /* Gather sparse, non-retired partial slabs as candidates. */
            btm_slab_t *cand[64];
            uint64_t occ[64][BTM_OCC_WORDS];
            int nc = 0;
            for (btm_slab_t *s = pool->partial; s && nc < 64; s = s->next) {
                if (s->retired || s->nslots > BTM_OCC_MAXSLOTS) continue;
                unsigned used = s->nslots - s->free_count;
                if (used == 0 || used * 2 > s->nslots) continue; /* not sparse */
                cand[nc] = s;
                build_occ(s, occ[nc]);
                nc++;
            }

            /* Greedy: for each donor, find a recipient with disjoint occupancy. */
            char used_flag[64] = {0};
            unsigned words = 0;
            for (int i = 0; i < nc; i++) {
                if (used_flag[i]) continue;
                for (int j = 0; j < nc; j++) {
                    if (i == j || used_flag[j]) continue;
                    if (cand[i]->nslots != cand[j]->nslots) continue;
                    words = (cand[i]->nslots + 63) / 64;
                    int disjoint = 1;
                    for (unsigned w = 0; w < words; w++)
                        if (occ[i][w] & occ[j][w]) { disjoint = 0; break; }
                    if (!disjoint) continue;
                    /* i = recipient, j = donor. */
                    reclaimed += mesh_pair(pool, cand[i], cand[j], occ[i], occ[j]);
                    used_flag[i] = used_flag[j] = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&pool->lock);
        }
    }
    return reclaimed;
}

/* ---- cold-data tiering ---- *
 *
 * Evict the data pages of "settled" slabs — full (no free slots) and not
 * touched by the allocator since the previous call — to swap via MADV_PAGEOUT.
 * Their objects fault back transparently on next access. This attacks "hotness
 * fragmentation" (cold-but-live data pinning DRAM): the allocator-activity
 * epoch is a proxy for coldness, and full+idle slabs are where long-lived data
 * settles. The caller decides cadence (e.g. on entering an idle period). */
size_t btm_pageout_cold(void) {
    if (!atomic_load_explicit(&btm_ready, memory_order_acquire)) return 0;
    uint32_t g = atomic_load_explicit(&btm_tier_epoch, memory_order_relaxed);
    size_t total = 0;

    for (unsigned p = 0; p < btm_nparts; p++) {
        for (int sc = 0; sc < BTM_NUM_SIZE_CLASSES; sc++) {
            btm_scpool_t *pool = &btm_partitions[p].pools[sc];
            pthread_mutex_lock(&pool->lock);
            for (btm_chunk_t *c = pool->chunks; c; c = c->next) {
                uint32_t page = BTM_DATA_START_PAGE;
                while (page < c->next_free_page) {
                    btm_slab_t *s = &btm_chunk_descs(c)[page];
                    uint32_t np = s->npages ? s->npages : 1;
                    if (s->free_count == 0 && !s->retired && !s->paged_out &&
                        s->epoch < g) {
                        size_t len = (size_t)np * BTM_PAGE_SIZE;
                        madvise(btm_slab_data(s), len, MADV_PAGEOUT);
                        s->paged_out = 1;
                        total += len;
                    }
                    page += np;
                }
            }
            pthread_mutex_unlock(&pool->lock);
        }
    }
    /* Advance the epoch so slabs touched before now count as idle next time. */
    atomic_fetch_add_explicit(&btm_tier_epoch, 1, memory_order_relaxed);
    return total;
}

void btm_pool_free_one(btm_slab_t *slab, void *ptr) {
    btm_scpool_t *pool = &btm_partitions[slab->part_idx].pools[slab->sc];
    pthread_mutex_lock(&pool->lock);
    btm_fl_set(ptr, slab->free_head);
    slab->free_head = ptr;
    slab->free_count++;
    slab_touch(slab);
    if (pool->outstanding) pool->outstanding--;
    if (slab->retired) {
        /* Meshed slab: inert. */
    } else if (slab->free_count == slab->nslots) {
        slab_became_free(pool, slab);
    } else if (!slab->in_partial) {
        partial_push(pool, slab);
    }
    pthread_mutex_unlock(&pool->lock);
}
