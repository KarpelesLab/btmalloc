/*
 * btmalloc.c — Phase A core: initialization, hot paths, and public API.
 *
 * Allocation flow:
 *   1. size -> size_class (small) or large path.
 *   2. partition = hash(return_address) mod P.
 *   3. cache bin for (partition, size_class): pop on hit; refill from the
 *      partition's slab pool on miss.
 * Free flow:
 *   1. resolve pointer -> owning chunk/large via the radix registry.
 *   2. small: recover (partition, size_class) from the slab, push to the
 *      matching cache bin (so the slot returns to its home partition).
 *   3. large: munmap.
 */

#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------------- globals ---------------- */

btm_partition_t *btm_partitions;
unsigned         btm_nparts = 1;
_Atomic(int)     btm_ready;

static pthread_once_t btm_init_once = PTHREAD_ONCE_INIT;

static unsigned read_nparts_env(void) {
    unsigned p = 64; /* default */
    const char *e = getenv("BTM_PARTITIONS");
    if (e && *e) {
        long v = atol(e);
        if (v >= 1) p = (unsigned)v;
    }
    unsigned np = 1;
    while (np < p) np <<= 1;          /* round up to a power of two */
    if (np > 4096) np = 4096;
    return np;
}

/* Child-side fork handler: only the forking thread exists in the child, so any
 * pool lock held by another thread is orphaned. Reinitialize all locks (safe:
 * the child is single-threaded here) and reset the background subsystem. */
static void btm_atfork_child(void) {
    for (unsigned i = 0; i < btm_nparts; i++)
        for (int sc = 0; sc < BTM_NUM_SIZE_CLASSES; sc++)
            pthread_mutex_init(&btm_partitions[i].pools[sc].lock, NULL);
    btm_bg_atfork_child();
}

static void btm_do_init(void) {
    btm_size_class_init();
    btm_chunk_init();

    unsigned np = read_nparts_env();
    size_t bytes = (size_t)np * sizeof(btm_partition_t);
    btm_partition_t *parts = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (parts == MAP_FAILED) {
        /* Last resort: a single partition in static storage would be nicer,
         * but OOM at init is unrecoverable. Leave btm_ready unset; callers
         * will keep retrying init (and failing) which surfaces the OOM. */
        return;
    }
    for (unsigned i = 0; i < np; i++)
        for (int sc = 0; sc < BTM_NUM_SIZE_CLASSES; sc++)
            pthread_mutex_init(&parts[i].pools[sc].lock, NULL);

    btm_partitions = parts;
    btm_nparts = np;
    pthread_atfork(NULL, NULL, btm_atfork_child);
    atomic_store_explicit(&btm_ready, 1, memory_order_release);
}

void btm_ensure_init(void) {
    if (BTM_LIKELY(atomic_load_explicit(&btm_ready, memory_order_acquire)))
        return;
    pthread_once(&btm_init_once, btm_do_init);
}

/* ---------------- hot paths ---------------- */

void *btm_malloc_at(size_t size, void *ra) {
    int sc = btm_size_to_sc(size);
    if (BTM_UNLIKELY(sc < 0)) {
        if (size == 0) return NULL;
        btm_ensure_init();
        return btm_large_alloc(size, BTM_ALIGNMENT);
    }

    btm_tls_t *t = btm_tls;
    if (BTM_UNLIKELY(t == NULL)) {
        btm_ensure_init();
        t = btm_tls_get();
        if (!t) return NULL;
    }

    unsigned part = btm_partition_of(ra);
    btm_tls_bin_t *bin = btm_tls_bin_at(t, part, sc);

    void *p = bin->free_head;
    if (BTM_LIKELY(p != NULL)) {
        bin->free_head = *(void **)p;
        bin->count--;
        return p;
    }

    bin->pool = &btm_partitions[part].pools[sc]; /* ensure flush can find it */
    unsigned want = btm_cache_max(sc) / 2;
    if (want < 1) want = 1;
    return btm_pool_refill(&btm_partitions[part], sc, bin, want);
}

void btm_free(void *ptr) {
    if (!ptr) return;

    btm_tls_t *t = btm_tls;
    uintptr_t owner = btm_resolve_owner(t, ptr);
    if (BTM_UNLIKELY(owner == 0)) {
        /* Foreign pointer / invalid free. Silently ignore (matches glibc with
         * checks disabled); a debug build can be made to abort here. */
        return;
    }

    if (*(uint64_t *)owner == BTM_CHUNK_MAGIC) {
        btm_chunk_t *c = (btm_chunk_t *)owner;
        btm_slab_t *slab = btm_slab_of(c, ptr);
        int sc = slab->sc;
        unsigned pidx = slab->part_idx; /* no pointer-division per free */

        if (BTM_UNLIKELY(t == NULL)) {
            t = btm_tls_get();
            if (!t) { btm_pool_free_one(slab, ptr); return; }
        }
        btm_tls_bin_t *bin = btm_tls_bin_at(t, pidx, sc);
        bin->pool = c->pool; /* the chunk's pool is exactly (pidx, sc)'s pool */
        *(void **)ptr = bin->free_head;
        bin->free_head = ptr;
        bin->count++;
        unsigned cap = btm_cache_max(sc);
        if (BTM_UNLIKELY(bin->count > cap)) btm_pool_flush(bin, cap / 2);
    } else {
        btm_large_free((void *)owner);
    }
}

void *btm_calloc_at(size_t n, size_t size, void *ra) {
    size_t bytes;
    if (__builtin_mul_overflow(n, size, &bytes)) return NULL;
    if (bytes == 0) return NULL;

    if (bytes > BTM_SMALL_MAX_SIZE) {
        btm_ensure_init();
        return btm_large_alloc(bytes, BTM_ALIGNMENT); /* mmap is zero-filled */
    }
    void *p = btm_malloc_at(bytes, ra);
    if (p) memset(p, 0, bytes);
    return p;
}

void *btm_realloc_at(void *ptr, size_t size, void *ra) {
    if (ptr == NULL) return btm_malloc_at(size, ra);
    if (size == 0) { btm_free(ptr); return NULL; }

    uintptr_t owner = btm_resolve_owner(btm_tls, ptr);
    if (BTM_UNLIKELY(owner == 0)) return NULL;

    if (*(uint64_t *)owner == BTM_CHUNK_MAGIC) {
        btm_chunk_t *c = (btm_chunk_t *)owner;
        btm_slab_t *slab = btm_slab_of(c, ptr);
        size_t old = btm_sc_to_size[slab->sc];
        int nsc = btm_size_to_sc(size);
        if (nsc == (int)slab->sc) return ptr; /* same class: no-op */

        void *np = btm_malloc_at(size, ra);
        if (!np) return NULL;
        memcpy(np, ptr, old < size ? old : size);
        btm_free(ptr);
        return np;
    }

    /* Source is a large allocation. */
    size_t old = btm_large_usable_size(owner, ptr);
    if (size <= BTM_SMALL_MAX_SIZE) {
        void *np = btm_malloc_at(size, ra);
        if (!np) return NULL;
        memcpy(np, ptr, old < size ? old : size);
        btm_large_free((void *)owner);
        return np;
    }
    return btm_large_realloc(owner, ptr, size);
}

/* ---------------- public API (capture the caller's return address) ---------------- */

void *btm_malloc(size_t size) {
    return btm_malloc_at(size, __builtin_return_address(0));
}

void *btm_calloc(size_t n, size_t size) {
    return btm_calloc_at(n, size, __builtin_return_address(0));
}

void *btm_realloc(void *ptr, size_t size) {
    return btm_realloc_at(ptr, size, __builtin_return_address(0));
}

void *btm_reallocarray(void *ptr, size_t n, size_t size) {
    size_t bytes;
    if (__builtin_mul_overflow(n, size, &bytes)) { errno = ENOMEM; return NULL; }
    return btm_realloc_at(ptr, bytes, __builtin_return_address(0));
}

static inline int is_pow2(size_t x) { return x && (x & (x - 1)) == 0; }

int btm_posix_memalign(void **out, size_t alignment, size_t size) {
    if (out == NULL) return EINVAL;
    if (!is_pow2(alignment) || (alignment % sizeof(void *)) != 0) return EINVAL;
    if (size == 0) { *out = NULL; return 0; }

    btm_ensure_init();
    void *p;
    if (alignment <= BTM_ALIGNMENT)
        p = btm_malloc_at(size, __builtin_return_address(0));
    else
        p = btm_large_alloc(size, alignment);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

void *btm_aligned_alloc(size_t alignment, size_t size) {
    if (!is_pow2(alignment)) { errno = EINVAL; return NULL; }
    if (size == 0) return NULL;
    if ((size % alignment) != 0) { errno = EINVAL; return NULL; } /* C11 */

    btm_ensure_init();
    if (alignment <= BTM_ALIGNMENT)
        return btm_malloc_at(size, __builtin_return_address(0));
    return btm_large_alloc(size, alignment);
}

size_t btm_malloc_usable_size(const void *ptr) {
    if (!ptr) return 0;
    uintptr_t owner = btm_resolve_owner(btm_tls, ptr);
    if (!owner) return 0;
    if (*(uint64_t *)owner == BTM_CHUNK_MAGIC) {
        btm_chunk_t *c = (btm_chunk_t *)owner;
        btm_slab_t *slab = btm_slab_of(c, ptr);
        return btm_sc_to_size[slab->sc];
    }
    return btm_large_usable_size(owner, ptr);
}
