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

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

/* ---------------- globals ---------------- */

btm_partition_t *btm_partitions;
unsigned         btm_nparts = 1;
_Atomic(int)     btm_ready;
int              btm_intern_mode;
int              btm_mesh_mode;
int              btm_harden_mode; /* BTM_HARDEN=1: double-free detection */
uintptr_t        btm_fl_key;      /* freelist safe-linking secret */
_Atomic(uint32_t) btm_tier_epoch; /* advanced by btm_pageout_cold */

static pthread_once_t btm_init_once = PTHREAD_ONCE_INIT;

/* ---- call-site interning (deterministic partition assignment) ---- *
 *
 * Open-addressing table mapping a return address to a monotonically assigned
 * id; partition = id mod nparts. Distinct call sites therefore get distinct
 * partitions until nparts is exhausted, after which they wrap and share. A
 * per-thread one-entry cache keeps the common (repeated call site) case to a
 * compare, so the global table is touched only on cold sites. */
typedef struct {
    _Atomic(uintptr_t) ra;
    unsigned           id;
} btm_intern_ent_t;

static btm_intern_ent_t *intern_tbl;
static unsigned          intern_cap;       /* power of two */
static _Atomic(unsigned) intern_next;      /* next id to assign */
static pthread_mutex_t   intern_lock = PTHREAD_MUTEX_INITIALIZER;

_Thread_local void    *btm_tls_intern_ra;
_Thread_local unsigned btm_tls_intern_part;

unsigned btm_intern_slow(void *ra) {
    uintptr_t key = (uintptr_t)ra;
    uintptr_t h = (key >> 4) * 0x9E3779B97F4A7C15ULL;
    unsigned mask = intern_cap - 1;
    unsigned i = (unsigned)(h >> 40) & mask;
    unsigned part;

    for (unsigned probe = 0; probe < intern_cap; probe++) {
        uintptr_t e = atomic_load_explicit(&intern_tbl[i].ra, memory_order_acquire);
        if (e == key) { part = intern_tbl[i].id & (btm_nparts - 1); goto done; }
        if (e == 0) {
            /* Empty slot: claim it under the lock (cold path, rare). */
            pthread_mutex_lock(&intern_lock);
            e = atomic_load_explicit(&intern_tbl[i].ra, memory_order_acquire);
            if (e == 0) {
                unsigned id = atomic_fetch_add_explicit(&intern_next, 1,
                                                        memory_order_relaxed);
                intern_tbl[i].id = id;
                atomic_store_explicit(&intern_tbl[i].ra, key, memory_order_release);
                pthread_mutex_unlock(&intern_lock);
                part = id & (btm_nparts - 1);
                goto done;
            }
            pthread_mutex_unlock(&intern_lock);
            if (e == key) { part = intern_tbl[i].id & (btm_nparts - 1); goto done; }
        }
        i = (i + 1) & mask;
    }
    /* Table full (more call sites than capacity): fall back to hashing. */
    part = (unsigned)(h >> 40) & (btm_nparts - 1);
done:
    btm_tls_intern_ra = ra;
    btm_tls_intern_part = part;
    return part;
}

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

    /* Seed the freelist safe-linking secret from the kernel CSPRNG. */
    if (getrandom(&btm_fl_key, sizeof btm_fl_key, 0) != (ssize_t)sizeof btm_fl_key) {
        /* Fallback: mix ASLR'd addresses and the clock — weaker but never 0. */
        btm_fl_key = (uintptr_t)&btm_fl_key ^ ((uintptr_t)&btm_do_init << 13);
        btm_fl_key ^= (uintptr_t)pthread_self() * 0x9E3779B97F4A7C15ull;
    }
    btm_fl_key |= 1; /* never zero */

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

    /* Optional memfd-backed chunks enabling Mesh-style compaction. Must be set
     * before any chunk is mapped. */
    {
        const char *m = getenv("BTM_MESH");
        if (m && m[0] == '1' && btm_mesh_enable()) btm_mesh_mode = 1;
    }

    /* Optional deterministic call-site -> partition interning. */
    {
        const char *h = getenv("BTM_HARDEN");
        if (h && h[0] == '1') btm_harden_mode = 1;
    }

    const char *mode = getenv("BTM_PARTITION_MODE");
    if (mode && (mode[0] == 'i' || mode[0] == 'I')) { /* "intern" */
        unsigned cap = 1;
        while (cap < np * 8u) cap <<= 1; /* room to keep probing cheap */
        if (cap < 1024) cap = 1024;
        intern_tbl = mmap(NULL, (size_t)cap * sizeof(btm_intern_ent_t),
                          PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE,
                          -1, 0);
        if (intern_tbl != MAP_FAILED) {
            intern_cap = cap;
            btm_intern_mode = 1;
        } else {
            intern_tbl = NULL;
        }
    }

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
        /* Start the maintenance thread here — no pool lock is held, so the
         * allocations pthread_create makes can't deadlock on one. */
        btm_bg_ensure_started();
    }

    unsigned part = btm_partition_of(ra);
    btm_tls_bin_t *bin = btm_tls_bin_at(t, part, sc);

    void *p = bin->free_head;
    if (BTM_LIKELY(p != NULL)) {
        bin->free_head = btm_fl_get(p);
        bin->count--;
        if (BTM_UNLIKELY(btm_harden_mode)) ((uintptr_t *)p)[1] = 0; /* clear canary */
        return p;
    }

    bin->pool = &btm_partitions[part].pools[sc]; /* ensure flush can find it */
    /* Record a representative call site for this partition (profiling). Only on
     * the refill path, so the fast path stays untouched. */
    if (BTM_UNLIKELY(btm_partitions[part].sample_ra == NULL))
        btm_partitions[part].sample_ra = ra;
    unsigned want = btm_cache_max(sc) / 2;
    if (want < 1) want = 1;
    p = btm_pool_refill(&btm_partitions[part], sc, bin, want);
    if (BTM_UNLIKELY(btm_harden_mode) && p) ((uintptr_t *)p)[1] = 0;
    return p;
}

/* Report a detected double-free, attributing it to the call site that fed the
 * containing slab's partition, then abort. */
__attribute__((noinline))
static void btm_double_free(void *ptr, btm_chunk_t *c) {
    void *ra = btm_partitions[c->part_idx].sample_ra;
    const char *sym = "?", *lib = "";
    Dl_info info;
    if (ra && dladdr(ra, &info)) {
        if (info.dli_sname) sym = info.dli_sname;
        if (info.dli_fname) lib = info.dli_fname;
    }
    char line[256];
    int n = snprintf(line, sizeof line,
                     "btmalloc: double free of %p (size class %u bytes); "
                     "partition is fed by ~%s (%p in %s)\n",
                     ptr, (unsigned)btm_sc_to_size[c->sc], sym, ra, lib);
    if (n > 0) { ssize_t w = write(2, line, (size_t)n); (void)w; }
    abort();
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

    if (BTM_UNLIKELY(owner & 1)) { /* tagged large allocation */
        btm_large_free((void *)(owner & ~(uintptr_t)1));
        return;
    }
    btm_chunk_t *c = (btm_chunk_t *)owner;
    /* The chunk is per-(partition, size_class), so it knows both directly — no
     * need to resolve the slab on the hot path (that's only for flush). */
    int sc = c->sc;
    unsigned pidx = c->part_idx;

    if (BTM_UNLIKELY(t == NULL)) {
        t = btm_tls_get();
        if (!t) { btm_pool_free_one(btm_slab_of(c, ptr), ptr); return; }
    }
    if (BTM_UNLIKELY(btm_harden_mode)) {
        /* Detect a (consecutive) double-free: the slot still carries the canary
         * stamped at its last free and not yet cleared by a reallocation. */
        if (((uintptr_t *)ptr)[1] == btm_df_canary(ptr)) btm_double_free(ptr, c);
    }

    btm_tls_bin_t *bin = btm_tls_bin_at(t, pidx, sc);
    bin->pool = c->pool; /* the chunk's pool is exactly (pidx, sc)'s pool */
    btm_fl_set(ptr, bin->free_head);
    bin->free_head = ptr;
    bin->count++;
    if (BTM_UNLIKELY(btm_harden_mode)) ((uintptr_t *)ptr)[1] = btm_df_canary(ptr);
    unsigned cap = btm_cache_max(sc);
    if (BTM_UNLIKELY(bin->count > cap)) btm_pool_flush(bin, cap / 2);
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

    if (!(owner & 1)) { /* small (chunk) source */
        btm_chunk_t *c = (btm_chunk_t *)owner;
        size_t old = btm_sc_to_size[c->sc];
        int nsc = btm_size_to_sc(size);
        if (nsc == (int)c->sc) return ptr; /* same class: no-op */

        void *np = btm_malloc_at(size, ra);
        if (!np) return NULL;
        memcpy(np, ptr, old < size ? old : size);
        btm_free(ptr);
        return np;
    }

    /* Source is a large allocation. */
    uintptr_t base = owner & ~(uintptr_t)1;
    size_t old = btm_large_usable_size(base, ptr);
    if (size <= BTM_SMALL_MAX_SIZE) {
        void *np = btm_malloc_at(size, ra);
        if (!np) return NULL;
        memcpy(np, ptr, old < size ? old : size);
        btm_large_free((void *)base);
        return np;
    }
    return btm_large_realloc(base, ptr, size);
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
    if (!(owner & 1)) {
        return btm_sc_to_size[((btm_chunk_t *)owner)->sc];
    }
    return btm_large_usable_size(owner & ~(uintptr_t)1, ptr);
}

void btm_heap_profile(int fd) {
    if (!atomic_load_explicit(&btm_ready, memory_order_acquire)) return;

    char line[256];
    int n = snprintf(line, sizeof line,
                     "btmalloc heap profile (P=%u partitions)\n"
                     "%-6s %14s  call site\n",
                     btm_nparts, "part", "bytes_outstanding");
    if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }

    uint64_t total = 0;
    for (unsigned i = 0; i < btm_nparts; i++) {
        uint64_t bytes = 0;
        for (int sc = 0; sc < BTM_NUM_SIZE_CLASSES; sc++) {
            /* Aligned 64-bit read; a slightly stale value is fine for a
             * profiler, so no lock is taken. */
            bytes += btm_partitions[i].pools[sc].outstanding *
                     (uint64_t)btm_sc_to_size[sc];
        }
        if (bytes == 0) continue;
        total += bytes;

        const char *sym = "?", *lib = "";
        void *ra = btm_partitions[i].sample_ra;
        Dl_info info;
        if (ra && dladdr(ra, &info)) {
            if (info.dli_sname) sym = info.dli_sname;
            if (info.dli_fname) lib = info.dli_fname;
        }
        n = snprintf(line, sizeof line, "%-6u %14llu  ~%s (%p in %s)\n",
                     i, (unsigned long long)bytes, sym, ra, lib);
        if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }
    }

    n = snprintf(line, sizeof line, "total outstanding (small): %llu bytes\n",
                 (unsigned long long)total);
    if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }
}
