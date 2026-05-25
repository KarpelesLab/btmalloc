/*
 * test_stress.c — randomized shadow-map stress test.
 *
 * Maintains a table of live allocations, each tagged with a recorded size and
 * a per-allocation magic byte written across the whole buffer. Randomly
 * allocates and frees; on every touch it verifies the buffer still holds the
 * expected magic end-to-end. This catches freelist-threading bugs, size-class
 * mislookups, slab header corruption, and (in threaded mode) races in the
 * cross-thread free path.
 *
 * Modes:
 *   single  : one thread, large op count.
 *   threads : N threads each own a disjoint slice; a fraction of frees are
 *             handed to a neighbor thread to exercise cross-thread frees.
 */

#include "btmalloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        abort(); \
    } \
} while (0)

static uint64_t rng(uint64_t *s) {
    uint64_t x = *s; x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x; return x * 0x2545F4914F6CDD1Dull;
}

/* Mixed size distribution: mostly small, some medium, a few large. */
static size_t pick_size(uint64_t *s) {
    uint64_t r = rng(s) % 100;
    if (r < 70) return 1 + rng(s) % 256;        /* small */
    if (r < 95) return 1 + rng(s) % 4096;        /* medium */
    return 1 + rng(s) % 200000;                  /* large (exercises mmap path) */
}

struct entry {
    void  *p;
    size_t sz;
    uint8_t magic;
};

static void fill(struct entry *e) {
    memset(e->p, e->magic, e->sz);
}
static void verify(struct entry *e) {
    const uint8_t *b = e->p;
    for (size_t i = 0; i < e->sz; i++) CHECK(b[i] == e->magic);
}

static void run_single(long ops, int slots, uint64_t seed) {
    struct entry *tbl = calloc((size_t)slots, sizeof(*tbl));
    CHECK(tbl);
    uint64_t s = seed;
    for (long i = 0; i < ops; i++) {
        int idx = (int)(rng(&s) % (uint64_t)slots);
        if (tbl[idx].p) {
            verify(&tbl[idx]);
            btm_free(tbl[idx].p);
            tbl[idx].p = NULL;
        } else {
            size_t sz = pick_size(&s);
            void *p = btm_malloc(sz);
            CHECK(p != NULL);
            tbl[idx].p = p;
            tbl[idx].sz = sz;
            tbl[idx].magic = (uint8_t)(rng(&s) | 1);
            fill(&tbl[idx]);
        }
    }
    for (int i = 0; i < slots; i++) if (tbl[i].p) { verify(&tbl[i]); btm_free(tbl[i].p); }
    free(tbl);
}

/* ---- threaded mode with cross-thread frees ---- */

#define XQ_SIZE 4096
struct xqueue {
    _Atomic(void *) slot[XQ_SIZE];
    atomic_uint head, tail;
};
static void xq_push(struct xqueue *q, void *p) {
    unsigned h = atomic_fetch_add_explicit(&q->head, 1, memory_order_relaxed);
    /* Drop if full (best-effort hand-off; we free locally instead). */
    void *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&q->slot[h & (XQ_SIZE - 1)],
            &expected, p, memory_order_release, memory_order_relaxed)) {
        btm_free(p); /* queue slot occupied; just free locally */
    }
}
static void *xq_pop(struct xqueue *q) {
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
    if ((t & (XQ_SIZE - 1)) == (h & (XQ_SIZE - 1))) return NULL;
    if (!atomic_compare_exchange_strong_explicit(&q->tail, &t, t + 1,
            memory_order_acq_rel, memory_order_relaxed))
        return NULL;
    void *p = atomic_exchange_explicit(&q->slot[t & (XQ_SIZE - 1)], NULL,
                                       memory_order_acquire);
    return p;
}

struct targ {
    long ops;
    int slots;
    uint64_t seed;
    struct xqueue *to_neighbor; /* push frees here */
    struct xqueue *from_neighbor;
};

static void *tworker(void *vp) {
    struct targ *a = vp;
    struct entry *tbl = calloc((size_t)a->slots, sizeof(*tbl));
    CHECK(tbl);
    uint64_t s = a->seed;
    for (long i = 0; i < a->ops; i++) {
        /* Drain any objects a neighbor handed us to free. */
        void *rp;
        while ((rp = xq_pop(a->from_neighbor)) != NULL) btm_free(rp);

        int idx = (int)(rng(&s) % (uint64_t)a->slots);
        if (tbl[idx].p) {
            verify(&tbl[idx]);
            if (a->to_neighbor && (rng(&s) % 4) == 0) {
                xq_push(a->to_neighbor, tbl[idx].p); /* cross-thread free */
            } else {
                btm_free(tbl[idx].p);
            }
            tbl[idx].p = NULL;
        } else {
            size_t sz = pick_size(&s);
            void *p = btm_malloc(sz);
            CHECK(p != NULL);
            tbl[idx].p = p; tbl[idx].sz = sz; tbl[idx].magic = (uint8_t)(rng(&s) | 1);
            fill(&tbl[idx]);
        }
    }
    void *rp;
    while ((rp = xq_pop(a->from_neighbor)) != NULL) btm_free(rp);
    for (int i = 0; i < a->slots; i++) if (tbl[i].p) { verify(&tbl[i]); btm_free(tbl[i].p); }
    free(tbl);
    return NULL;
}

static void run_threads(int n, long ops, int slots) {
    if (n > 16) n = 16;
    pthread_t th[16];
    struct targ args[16];
    struct xqueue *qs = calloc((size_t)n, sizeof(struct xqueue));
    CHECK(qs);
    for (int i = 0; i < n; i++) {
        args[i].ops = ops;
        args[i].slots = slots;
        args[i].seed = 0xF00D ^ ((uint64_t)(i + 1) * 0x9E3779B1u);
        args[i].to_neighbor = &qs[(i + 1) % n];
        args[i].from_neighbor = &qs[i];
    }
    for (int i = 0; i < n; i++) pthread_create(&th[i], NULL, tworker, &args[i]);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    free(qs);
}

int main(int argc, char **argv) {
    long ops = (argc > 1) ? atol(argv[1]) : 1000000;

    printf("stress: single-threaded (%ld ops)...\n", ops);
    run_single(ops, 4096, 0x12345);

    printf("stress: 8 threads with cross-thread frees (%ld ops each)...\n", ops / 2);
    run_threads(8, ops / 2, 2048);

    printf("stress: PASS\n");
    return 0;
}
