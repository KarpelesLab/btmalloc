/*
 * bench_threaded — multi-threaded scaling and cross-thread free.
 *
 * Mode "local":     each thread allocs+frees its own objects (allocator-
 *                   friendly; measures raw scalability of the fast path).
 * Mode "prodcons":  half the threads allocate and hand objects to the other
 *                   half through a shared ring; consumers free. Stresses the
 *                   cross-thread / remote-free path that PC-anchored placement
 *                   is meant to handle well.
 *
 * Reports aggregate Mops/sec (million ops/sec) for thread counts 1..32.
 * Higher is better.
 */

#include "bench_common.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static const int kThreads[] = {1, 2, 4, 8, 16, 32};
#define NTC (sizeof(kThreads) / sizeof(kThreads[0]))

static const size_t kMixSizes[] = {16, 24, 32, 48, 64, 96, 128, 256, 512, 1024};
#define NMIX (sizeof(kMixSizes) / sizeof(kMixSizes[0]))

/* ---------------- local mode ---------------- */

struct local_arg {
    long ops;
    int  slots;
    uint64_t seed;
};

static void *local_worker(void *vp) {
    struct local_arg *a = vp;
    void **live = bench_meta_alloc((size_t)a->slots * sizeof(void *));
    memset(live, 0, (size_t)a->slots * sizeof(void *));
    uint64_t s = a->seed;
    for (long i = 0; i < a->ops; i++) {
        int idx = (int)(bench_rng(&s) % (uint64_t)a->slots);
        if (live[idx]) {
            free(live[idx]);
            live[idx] = NULL;
        } else {
            size_t sz = kMixSizes[bench_rng(&s) % NMIX];
            void *p = malloc(sz);
            ((volatile char *)p)[0] = 1;
            live[idx] = p;
        }
    }
    for (int i = 0; i < a->slots; i++) free(live[i]);
    bench_meta_free(live, (size_t)a->slots * sizeof(void *));
    return NULL;
}

static void run_local(int nthreads, long ops_per_thread) {
    pthread_t th[64];
    struct local_arg args[64];
    uint64_t t0 = bench_now_ns();
    for (int i = 0; i < nthreads; i++) {
        args[i].ops = ops_per_thread;
        args[i].slots = 256;
        args[i].seed = 0x1234567 ^ ((uint64_t)(i + 1) * 0x9E3779B1u);
        pthread_create(&th[i], NULL, local_worker, &args[i]);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    uint64_t t1 = bench_now_ns();

    double total_ops = (double)ops_per_thread * nthreads;
    double mops = total_ops / ((double)(t1 - t0) / 1e9) / 1e6;
    char param[32];
    snprintf(param, sizeof param, "threads=%d", nthreads);
    bench_emit("thr_local", param, "Mops_per_sec", mops);
}

/* ---------------- producer/consumer mode ---------------- */

#define RING_BITS 16
#define RING_SIZE (1 << RING_BITS)
#define RING_MASK (RING_SIZE - 1)

struct ring {
    _Atomic(void *) slot[RING_SIZE];
    atomic_uint     head; /* producer index */
    atomic_uint     tail; /* consumer index */
    atomic_int      producers_done;
};

struct pc_arg {
    struct ring *ring;
    long ops;
    uint64_t seed;
    int nproducers;
};

static void *producer(void *vp) {
    struct pc_arg *a = vp;
    uint64_t s = a->seed;
    for (long i = 0; i < a->ops; i++) {
        size_t sz = kMixSizes[bench_rng(&s) % NMIX];
        void *p = malloc(sz);
        ((volatile char *)p)[0] = 1;
        /* Spin-push into the ring. */
        for (;;) {
            unsigned h = atomic_load_explicit(&a->ring->head, memory_order_relaxed);
            unsigned t = atomic_load_explicit(&a->ring->tail, memory_order_acquire);
            if (((h + 1) & RING_MASK) == (t & RING_MASK)) { sched_yield(); continue; }
            if (atomic_compare_exchange_weak_explicit(&a->ring->head, &h, h + 1,
                    memory_order_acq_rel, memory_order_relaxed)) {
                atomic_store_explicit(&a->ring->slot[h & RING_MASK], p,
                                      memory_order_release);
                break;
            }
        }
    }
    return NULL;
}

static void *consumer(void *vp) {
    struct pc_arg *a = vp;
    for (;;) {
        unsigned t = atomic_load_explicit(&a->ring->tail, memory_order_relaxed);
        unsigned h = atomic_load_explicit(&a->ring->head, memory_order_acquire);
        if ((t & RING_MASK) == (h & RING_MASK)) {
            if (atomic_load_explicit(&a->ring->producers_done, memory_order_acquire)
                >= a->nproducers)
                break;
            sched_yield();
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&a->ring->tail, &t, t + 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            void *p;
            do {
                p = atomic_load_explicit(&a->ring->slot[t & RING_MASK],
                                         memory_order_acquire);
            } while (p == NULL);
            atomic_store_explicit(&a->ring->slot[t & RING_MASK], NULL,
                                  memory_order_relaxed);
            free(p);
        }
    }
    return NULL;
}

static void run_prodcons(int nthreads, long ops_per_thread) {
    if (nthreads < 2) return; /* needs at least one of each */
    struct ring *ring = bench_meta_alloc(sizeof(*ring));
    memset(ring, 0, sizeof(*ring));

    int nprod = nthreads / 2;
    int ncons = nthreads - nprod;
    pthread_t th[64];
    struct pc_arg pa = {ring, ops_per_thread, 0, nprod};
    struct pc_arg ca = {ring, 0, 0, nprod};

    uint64_t t0 = bench_now_ns();
    struct pc_arg pargs[64];
    for (int i = 0; i < nprod; i++) {
        pargs[i] = pa;
        pargs[i].seed = 0xABCDEFull ^ ((uint64_t)(i + 1) * 0x9E3779B1u);
        pthread_create(&th[i], NULL, producer, &pargs[i]);
    }
    for (int i = 0; i < ncons; i++)
        pthread_create(&th[nprod + i], NULL, consumer, &ca);

    for (int i = 0; i < nprod; i++) pthread_join(th[i], NULL);
    atomic_store_explicit(&ring->producers_done, nprod, memory_order_release);
    for (int i = 0; i < ncons; i++) pthread_join(th[nprod + i], NULL);
    uint64_t t1 = bench_now_ns();

    double total_ops = (double)ops_per_thread * nprod; /* alloc+free pairs */
    double mops = (2.0 * total_ops) / ((double)(t1 - t0) / 1e9) / 1e6;
    char param[32];
    snprintf(param, sizeof param, "threads=%d", nthreads);
    bench_emit("thr_prodcons", param, "Mops_per_sec", mops);

    bench_meta_free(ring, sizeof(*ring));
}

int main(int argc, char **argv) {
    long ops = (argc > 1) ? atol(argv[1]) : 2000000;
    for (size_t i = 0; i < NTC; i++) run_local(kThreads[i], ops);
    for (size_t i = 0; i < NTC; i++) run_prodcons(kThreads[i], ops);
    return 0;
}
