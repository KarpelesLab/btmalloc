/*
 * background.c — asynchronous backing-store management (Phase C).
 *
 * Two mechanisms keep slow kernel work off the allocating/freeing threads:
 *
 *  1. A global WARM-CHUNK POOL. Drained chunks are recycled instead of
 *     unmapped, and new chunks are taken from the pool, so steady-state
 *     allocation does no mmap/munmap at all. Chunks stay mapped and registered
 *     while warm; only their physical pages are released (MADV_DONTNEED).
 *
 *  2. A BACKGROUND MAINTENANCE THREAD. Disposed chunks are queued and the
 *     thread performs their MADV_DONTNEED via batched io_uring submissions,
 *     refills the pool ahead of demand (so an allocating thread rarely has to
 *     mmap on its own), and trims the pool back to the OS when it grows too
 *     large. io_uring (IORING_OP_MADVISE) lets many advises ride one syscall.
 *
 * io_uring has no mmap/munmap opcode, so those remain real syscalls — but the
 * warm pool removes them from the steady-state path, and the background thread
 * absorbs the rest. Set BTM_NO_ASYNC=1 to force the synchronous fallback.
 */

#include "internal.h"

#include <fcntl.h>
#include <liburing.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define BTM_WARM_LOW    8    /* refill the pool below this */
#define BTM_WARM_HIGH   64   /* trim the pool above this */
#define BTM_URING_DEPTH 256

static pthread_mutex_t bg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  bg_cv = PTHREAD_COND_INITIALIZER;
static btm_chunk_t    *warm_head;      /* stack of ready chunks, via ->next */
static int             warm_count;
static btm_chunk_t    *dispose_head;   /* chunks pending MADV_DONTNEED, via ->next */

static atomic_int      bg_started;     /* thread create attempted */
static int             bg_async;       /* route disposals to the thread */
static pthread_mutex_t bg_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       bg_thread;
static struct io_uring bg_ring;
static int             bg_ring_ok;

static const size_t kData = BTM_CHUNK_SIZE - BTM_PAGE_SIZE; /* pages 1..511 */

/* ---- mesh-mode memfd backing ---- *
 *
 * In mesh mode chunks are backed by a single memfd (one fd) so that two slabs
 * with non-overlapping live slots can later be remapped (MAP_FIXED) onto one
 * physical page. Each chunk occupies a CHUNK_SIZE-aligned offset in the memfd;
 * offsets are bump-allocated and recycled through a small free stack. Physical
 * pages are released with fallocate(PUNCH_HOLE) instead of MADV_DONTNEED. */
static int             memfd = -1;
static uint64_t        memfd_bump;          /* next never-used offset */
static uint64_t        memfd_size;          /* current ftruncate size */
static uint64_t       *memfd_free;          /* stack of recycled offsets */
static unsigned        memfd_free_n, memfd_free_cap;
static pthread_mutex_t memfd_lock = PTHREAD_MUTEX_INITIALIZER;

int btm_mesh_enable(void) {
    int fd = memfd_create("btmalloc", MFD_CLOEXEC);
    if (fd < 0) return 0;
    memfd = fd;
    memfd_bump = 0;
    memfd_size = 0;
    return 1;
}

/* Allocate a CHUNK_SIZE region in the memfd; returns its offset or UINT64_MAX. */
static uint64_t memfd_alloc_off(void) {
    pthread_mutex_lock(&memfd_lock);
    uint64_t off;
    if (memfd_free_n > 0) {
        off = memfd_free[--memfd_free_n];
    } else {
        off = memfd_bump;
        memfd_bump += BTM_CHUNK_SIZE;
        if (memfd_bump > memfd_size) {
            uint64_t want = memfd_size + 256 * BTM_CHUNK_SIZE; /* grow in slabs */
            if (want < memfd_bump) want = memfd_bump;
            if (ftruncate(memfd, (off_t)want) == 0) memfd_size = want;
            else { memfd_bump -= BTM_CHUNK_SIZE; off = UINT64_MAX; }
        }
    }
    pthread_mutex_unlock(&memfd_lock);
    return off;
}

static void memfd_free_off(uint64_t off) {
    pthread_mutex_lock(&memfd_lock);
    if (memfd_free_n == memfd_free_cap) {
        unsigned ncap = memfd_free_cap ? memfd_free_cap * 2 : 64;
        uint64_t *n = mmap(NULL, ncap * sizeof(uint64_t), PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (n != MAP_FAILED) {
            for (unsigned i = 0; i < memfd_free_n; i++) n[i] = memfd_free[i];
            if (memfd_free) munmap(memfd_free, memfd_free_cap * sizeof(uint64_t));
            memfd_free = n;
            memfd_free_cap = ncap;
        }
    }
    if (memfd_free_n < memfd_free_cap) memfd_free[memfd_free_n++] = off;
    /* else: drop the offset (leak in the memfd address space; rare). */
    pthread_mutex_unlock(&memfd_lock);
}

/* Release a chunk's data pages back to the OS. */
static void chunk_decommit(btm_chunk_t *c) {
    if (btm_mesh_mode) {
        fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                  (off_t)(c->memfd_off + BTM_PAGE_SIZE), (off_t)kData);
    } else {
        madvise((char *)c + BTM_PAGE_SIZE, kData, MADV_DONTNEED);
    }
}

/* ---- low-level chunk map / header ---- */

static btm_chunk_t *map_fresh(void) {
    uint64_t off = 0;
    if (btm_mesh_mode) {
        off = memfd_alloc_off();
        if (off == UINT64_MAX) return NULL;
    }

    /* Reserve a CHUNK_SIZE-aligned virtual range. */
    size_t want = BTM_CHUNK_SIZE * 2;
    void *raw = mmap(NULL, want, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (raw == MAP_FAILED) { if (btm_mesh_mode) memfd_free_off(off); return NULL; }
    uintptr_t base = ((uintptr_t)raw + BTM_CHUNK_SIZE - 1) & ~(BTM_CHUNK_SIZE - 1);
    size_t front = base - (uintptr_t)raw;
    if (front) munmap(raw, front);
    size_t back = want - front - BTM_CHUNK_SIZE;
    if (back) munmap((void *)(base + BTM_CHUNK_SIZE), back);

    void *p;
    if (btm_mesh_mode) {
        /* Back the aligned range with the memfd so it can be meshed later. */
        p = mmap((void *)base, BTM_CHUNK_SIZE, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED, memfd, (off_t)off);
    } else {
        p = mmap((void *)base, BTM_CHUNK_SIZE, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    }
    if (p == MAP_FAILED) {
        munmap((void *)base, BTM_CHUNK_SIZE);
        if (btm_mesh_mode) memfd_free_off(off);
        return NULL;
    }

    madvise((void *)base, BTM_CHUNK_SIZE, MADV_HUGEPAGE);
    if (btm_mesh_mode) {
        /* MAP_SHARED memfd memory would be shared with a fork() child and let
         * it corrupt the parent's heap. MADV_DONTFORK drops it from the child
         * so a child heap access faults loudly instead. Mesh mode is therefore
         * not safe across fork-without-exec; see docs. */
        madvise((void *)base, BTM_CHUNK_SIZE, MADV_DONTFORK);
    }
    btm_registry_insert(base, BTM_CHUNK_SIZE, base);
    btm_chunk_t *c = (btm_chunk_t *)base;
    c->memfd_off = off;
    return c;
}

static void chunk_init_header(btm_chunk_t *c, btm_scpool_t *pool) {
    c->magic = BTM_CHUNK_MAGIC;
    c->pool = pool;
    c->next = c->prev = NULL;
    c->next_free_page = 1; /* page 0 is the header */
    c->live_slabs = 0;
    for (unsigned i = 0; i < BTM_PAGES_PER_CHUNK; i++)
        c->page_owner[i] = BTM_PAGE_NONE;
}

/* ---- decommit (io_uring-batched madvise, else synchronous) ---- */

static void decommit_batch(btm_chunk_t *list) {
    /* Mesh mode releases memfd pages with fallocate(PUNCH_HOLE), which has no
     * io_uring fast path here; do it synchronously per chunk. */
    if (btm_mesh_mode || !bg_ring_ok) {
        for (btm_chunk_t *c = list; c; c = c->next) chunk_decommit(c);
        return;
    }
    unsigned pending = 0;
    for (btm_chunk_t *c = list; c; c = c->next) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&bg_ring);
        if (!sqe) {
            io_uring_submit(&bg_ring);
            struct io_uring_cqe *cqe;
            while (pending && io_uring_wait_cqe(&bg_ring, &cqe) == 0) {
                io_uring_cqe_seen(&bg_ring, cqe);
                pending--;
            }
            sqe = io_uring_get_sqe(&bg_ring);
            if (!sqe) { chunk_decommit(c); continue; } /* shouldn't happen */
        }
        io_uring_prep_madvise(sqe, (char *)c + BTM_PAGE_SIZE, kData, MADV_DONTNEED);
        pending++;
    }
    if (pending) {
        io_uring_submit(&bg_ring);
        struct io_uring_cqe *cqe;
        while (pending && io_uring_wait_cqe(&bg_ring, &cqe) == 0) {
            io_uring_cqe_seen(&bg_ring, cqe);
            pending--;
        }
    }
}

/* ---- background thread ---- */

static void *bg_main(void *arg) {
    (void)arg;
    if (io_uring_queue_init(BTM_URING_DEPTH, &bg_ring, 0) == 0) bg_ring_ok = 1;

    for (;;) {
        pthread_mutex_lock(&bg_lock);
        while (!dispose_head && warm_count >= BTM_WARM_LOW)
            pthread_cond_wait(&bg_cv, &bg_lock);
        btm_chunk_t *disp = dispose_head;
        dispose_head = NULL;
        pthread_mutex_unlock(&bg_lock);

        /* Release physical pages of all disposed chunks. */
        if (disp) decommit_batch(disp);

        /* Return cleaned chunks to the warm pool; trim the overflow to the OS. */
        btm_chunk_t *trim = NULL;
        pthread_mutex_lock(&bg_lock);
        while (disp) {
            btm_chunk_t *next = disp->next;
            if (warm_count < BTM_WARM_HIGH) {
                disp->next = warm_head;
                warm_head = disp;
                warm_count++;
            } else {
                disp->next = trim;
                trim = disp;
            }
            disp = next;
        }
        int deficit = BTM_WARM_LOW - warm_count;
        pthread_mutex_unlock(&bg_lock);

        while (trim) {
            btm_chunk_t *next = trim->next;
            uint64_t off = trim->memfd_off;
            btm_registry_remove((uintptr_t)trim, BTM_CHUNK_SIZE);
            munmap(trim, BTM_CHUNK_SIZE);
            if (btm_mesh_mode) memfd_free_off(off);
            trim = next;
        }

        /* Pre-map ahead of demand so allocators rarely mmap on their own. */
        for (int i = 0; i < deficit; i++) {
            btm_chunk_t *c = map_fresh();
            if (!c) break;
            pthread_mutex_lock(&bg_lock);
            c->next = warm_head;
            warm_head = c;
            warm_count++;
            pthread_mutex_unlock(&bg_lock);
        }
    }
    return NULL;
}

static void bg_kick(void) {
    if (atomic_load_explicit(&bg_started, memory_order_acquire)) {
        pthread_mutex_lock(&bg_lock);
        pthread_cond_signal(&bg_cv);
        pthread_mutex_unlock(&bg_lock);
        return;
    }
    pthread_mutex_lock(&bg_start_lock);
    if (!atomic_load_explicit(&bg_started, memory_order_acquire)) {
        atomic_store_explicit(&bg_started, 1, memory_order_release); /* block re-entry */
        if (!getenv("BTM_NO_ASYNC") &&
            pthread_create(&bg_thread, NULL, bg_main, NULL) == 0) {
            bg_async = 1;
        }
    }
    pthread_mutex_unlock(&bg_start_lock);
}

/* After fork(), only the calling thread survives into the child: the
 * maintenance thread is gone and bg_lock may be held by a thread that no longer
 * exists. Reset the async state so the child starts a fresh thread on demand;
 * the warm pool's chunks remain valid (COW) and are kept. */
void btm_bg_atfork_child(void) {
    pthread_mutex_init(&bg_lock, NULL);
    pthread_mutex_init(&bg_start_lock, NULL);
    pthread_cond_init(&bg_cv, NULL);
    dispose_head = NULL;
    bg_async = 0;
    bg_ring_ok = 0; /* the parent's io_uring ring is not usable in the child */
    atomic_store_explicit(&bg_started, 0, memory_order_release);
}

/* Mesh: remap the donor slab's data region onto the recipient's (so the
 * donor's virtual addresses now read the recipient's physical pages, where its
 * live objects were just copied), then punch the donor's own backing pages.
 * Both slabs have identical geometry (same size class, header in page 0, data
 * in pages 1..npages-1). Returns bytes of physical memory released. Caller must
 * hold the pool lock and guarantee no concurrent access to the donor's objects
 * (heap quiescent). */
size_t btm_mesh_remap(btm_slab_t *donor, btm_slab_t *recipient) {
    btm_chunk_t *dc = (btm_chunk_t *)((uintptr_t)donor & ~(BTM_CHUNK_SIZE - 1));
    btm_chunk_t *rc = (btm_chunk_t *)((uintptr_t)recipient & ~(BTM_CHUNK_SIZE - 1));
    unsigned dfp = (unsigned)(((uintptr_t)donor - (uintptr_t)dc) >> BTM_PAGE_SHIFT);
    unsigned rfp = (unsigned)(((uintptr_t)recipient - (uintptr_t)rc) >> BTM_PAGE_SHIFT);
    unsigned data_pages = donor->npages - 1; /* page 0 is the header */
    size_t len = (size_t)data_pages * BTM_PAGE_SIZE;

    uintptr_t donor_data = (uintptr_t)donor + BTM_PAGE_SIZE;
    uint64_t recip_off = rc->memfd_off + (uint64_t)(rfp + 1) * BTM_PAGE_SIZE;
    uint64_t donor_off = dc->memfd_off + (uint64_t)(dfp + 1) * BTM_PAGE_SIZE;

    void *r = mmap((void *)donor_data, len, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED, memfd, (off_t)recip_off);
    if (r == MAP_FAILED) return 0;
    madvise((void *)donor_data, len, MADV_DONTFORK);
    fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
              (off_t)donor_off, (off_t)len);
    return len;
}

/* ---- public obtain / dispose ---- */

btm_chunk_t *btm_chunk_obtain(btm_scpool_t *pool) {
    pthread_mutex_lock(&bg_lock);
    btm_chunk_t *c = warm_head;
    if (c) { warm_head = c->next; warm_count--; }
    int low = warm_count < BTM_WARM_LOW;
    pthread_mutex_unlock(&bg_lock);

    if (!c) c = map_fresh(); /* synchronous fallback when the pool is empty */
    if (!c) return NULL;
    if (low) bg_kick();

    chunk_init_header(c, pool);
    return c;
}

void btm_chunk_dispose(btm_chunk_t *c) {
    if (bg_async) {
        pthread_mutex_lock(&bg_lock);
        c->next = dispose_head;
        dispose_head = c;
        pthread_cond_signal(&bg_cv);
        pthread_mutex_unlock(&bg_lock);
        return;
    }
    /* Synchronous fallback: release pages here, recycle or trim inline. */
    chunk_decommit(c);
    uint64_t off = c->memfd_off;
    pthread_mutex_lock(&bg_lock);
    int warm = warm_count < BTM_WARM_HIGH;
    if (warm) { c->next = warm_head; warm_head = c; warm_count++; }
    pthread_mutex_unlock(&bg_lock);
    if (!warm) {
        btm_registry_remove((uintptr_t)c, BTM_CHUNK_SIZE);
        munmap(c, BTM_CHUNK_SIZE);
        if (btm_mesh_mode) memfd_free_off(off);
    }
    bg_kick(); /* try to bring the maintenance thread up for next time */
}
