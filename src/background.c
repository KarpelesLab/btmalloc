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

/* ---- low-level chunk map / header ---- */

static btm_chunk_t *map_fresh(void) {
    size_t want = BTM_CHUNK_SIZE * 2;
    void *raw = mmap(NULL, want, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (raw == MAP_FAILED) return NULL;
    uintptr_t base = ((uintptr_t)raw + BTM_CHUNK_SIZE - 1) & ~(BTM_CHUNK_SIZE - 1);
    size_t front = base - (uintptr_t)raw;
    if (front) munmap(raw, front);
    size_t back = want - front - BTM_CHUNK_SIZE;
    if (back) munmap((void *)(base + BTM_CHUNK_SIZE), back);
    madvise((void *)base, BTM_CHUNK_SIZE, MADV_HUGEPAGE);
    btm_registry_insert(base, BTM_CHUNK_SIZE, base);
    return (btm_chunk_t *)base;
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

/* ---- madvise (io_uring batched, else synchronous) ---- */

static void madvise_one(btm_chunk_t *c) {
    madvise((char *)c + BTM_PAGE_SIZE, kData, MADV_DONTNEED);
}

static void madvise_batch(btm_chunk_t *list) {
    if (!bg_ring_ok) {
        for (btm_chunk_t *c = list; c; c = c->next) madvise_one(c);
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
            if (!sqe) { madvise_one(c); continue; } /* shouldn't happen */
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

        /* Release physical pages of all disposed chunks in one io_uring batch. */
        if (disp) madvise_batch(disp);

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
            btm_registry_remove((uintptr_t)trim, BTM_CHUNK_SIZE);
            munmap(trim, BTM_CHUNK_SIZE);
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
    madvise_one(c);
    pthread_mutex_lock(&bg_lock);
    int warm = warm_count < BTM_WARM_HIGH;
    if (warm) { c->next = warm_head; warm_head = c; warm_count++; }
    pthread_mutex_unlock(&bg_lock);
    if (!warm) {
        btm_registry_remove((uintptr_t)c, BTM_CHUNK_SIZE);
        munmap(c, BTM_CHUNK_SIZE);
    }
    bg_kick(); /* try to bring the maintenance thread up for next time */
}
