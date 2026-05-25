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

/* ---------------- producer/consumer mode ----------------
 *
 * A mutex+condvar bounded MPMC queue. Producers allocate and enqueue;
 * consumers dequeue and free. The point is to exercise the cross-thread free
 * path (objects allocated by one thread, freed by another), not to be a
 * lock-free showcase — so simplicity and obvious correctness win here. */

#define PCQ_CAP 8192

struct pcq {
    void           *buf[PCQ_CAP];
    int             head, tail, count;
    int             producers_left;
    pthread_mutex_t mtx;
    pthread_cond_t  not_full, not_empty;
};

static void pcq_init(struct pcq *q, int nprod) {
    q->head = q->tail = q->count = 0;
    q->producers_left = nprod;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void pcq_push(struct pcq *q, void *p) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == PCQ_CAP) pthread_cond_wait(&q->not_full, &q->mtx);
    q->buf[q->head] = p;
    q->head = (q->head + 1) % PCQ_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

/* Returns an item, or NULL once the queue is drained and all producers done. */
static void *pcq_pop(struct pcq *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0 && q->producers_left > 0)
        pthread_cond_wait(&q->not_empty, &q->mtx);
    if (q->count == 0) { pthread_mutex_unlock(&q->mtx); return NULL; }
    void *p = q->buf[q->tail];
    q->tail = (q->tail + 1) % PCQ_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return p;
}

static void pcq_producer_done(struct pcq *q) {
    pthread_mutex_lock(&q->mtx);
    q->producers_left--;
    if (q->producers_left == 0) pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

struct pc_arg { struct pcq *q; long ops; uint64_t seed; };

static void *producer(void *vp) {
    struct pc_arg *a = vp;
    uint64_t s = a->seed;
    for (long i = 0; i < a->ops; i++) {
        void *p = malloc(kMixSizes[bench_rng(&s) % NMIX]);
        ((volatile char *)p)[0] = 1;
        pcq_push(a->q, p);
    }
    pcq_producer_done(a->q);
    return NULL;
}

static void *consumer(void *vp) {
    struct pc_arg *a = vp;
    void *p;
    while ((p = pcq_pop(a->q)) != NULL) free(p);
    return NULL;
}

static void run_prodcons(int nthreads, long ops_per_thread) {
    if (nthreads < 2) return; /* needs at least one of each */
    struct pcq *q = bench_meta_alloc(sizeof(*q));
    int nprod = nthreads / 2;
    int ncons = nthreads - nprod;
    pcq_init(q, nprod);

    pthread_t th[64];
    struct pc_arg pargs[64], ca = {q, 0, 0};

    uint64_t t0 = bench_now_ns();
    for (int i = 0; i < nprod; i++) {
        pargs[i].q = q;
        pargs[i].ops = ops_per_thread;
        pargs[i].seed = 0xABCDEFull ^ ((uint64_t)(i + 1) * 0x9E3779B1u);
        pthread_create(&th[i], NULL, producer, &pargs[i]);
    }
    for (int i = 0; i < ncons; i++)
        pthread_create(&th[nprod + i], NULL, consumer, &ca);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    uint64_t t1 = bench_now_ns();

    double total_ops = (double)ops_per_thread * nprod;
    double mops = (2.0 * total_ops) / ((double)(t1 - t0) / 1e9) / 1e6;
    char param[32];
    snprintf(param, sizeof param, "threads=%d", nthreads);
    bench_emit("thr_prodcons", param, "Mops_per_sec", mops);

    bench_meta_free(q, sizeof(*q));
}

int main(int argc, char **argv) {
    long ops = (argc > 1) ? atol(argv[1]) : 2000000;
    for (size_t i = 0; i < NTC; i++) run_local(kThreads[i], ops);
    for (size_t i = 0; i < NTC; i++) run_prodcons(kThreads[i], ops);
    return 0;
}
