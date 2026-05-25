/*
 * tcache.c — per-thread allocation cache.
 *
 * A thread's cache is a direct-mapped table of bins, each keyed by
 * (partition, size_class). The hot path (in btmalloc.c) reads btm_tls and the
 * matching bin directly; this file handles lazy creation, bin claiming on a
 * key miss, and flushing cached slots back to their pools at thread exit.
 *
 * Keying bins by (partition, size_class) — not just size_class — is what
 * preserves call-site segregation: a freed slot is cached under ITS partition
 * (recovered from its slab) and only ever reused for the same partition.
 */

#include "internal.h"

#include <sys/mman.h>

_Thread_local btm_tls_t *btm_tls;

static pthread_key_t  btm_tls_key;
static pthread_once_t btm_tls_key_once = PTHREAD_ONCE_INIT;

/* Total bytes for a per-thread cache: header + one bin per (partition, sc).
 * The array is large but mmap-lazy, so untouched bins never fault in. */
static size_t btm_tls_bytes(void) {
    return sizeof(btm_tls_t) +
           (size_t)btm_nparts * BTM_NUM_SIZE_CLASSES * sizeof(btm_tls_bin_t);
}

/* Flush every populated bin back to its home pool, then unmap. Runs at thread
 * exit while btm_partitions is still valid. */
static void btm_tls_destroy(void *p) {
    btm_tls_t *t = p;
    if (!t) return;
    btm_tls = NULL; /* the thread is leaving; don't let stale ptr be reused */
    unsigned nbins = btm_nparts * BTM_NUM_SIZE_CLASSES;
    for (unsigned i = 0; i < nbins; i++) {
        if (t->bins[i].count && t->bins[i].pool)
            btm_pool_flush(&t->bins[i], 0);
    }
    munmap(t, btm_tls_bytes());
}

static void btm_tls_make_key(void) {
    pthread_key_create(&btm_tls_key, btm_tls_destroy);
}

btm_tls_t *btm_tls_get(void) {
    btm_tls_t *t = btm_tls;
    if (BTM_LIKELY(t != NULL)) return t;

    pthread_once(&btm_tls_key_once, btm_tls_make_key);
    t = mmap(NULL, btm_tls_bytes(), PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (t == MAP_FAILED) return NULL; /* mmap zeroes: all bins start empty */
    btm_tls = t;
    pthread_setspecific(btm_tls_key, t);
    return t;
}
