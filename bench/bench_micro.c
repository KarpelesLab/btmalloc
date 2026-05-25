/*
 * bench_micro — single-threaded allocation throughput per size class.
 *
 * Three access patterns per size:
 *   churn  : malloc then immediately free (best case for any cache/magazine)
 *   lifo   : allocate N, free in reverse order (stack-like)
 *   fifo   : allocate N, free in allocation order (queue-like)
 *   random : allocate N, free in a shuffled order (fragmentation stress)
 *
 * Reports ns/op for each. Lower is better.
 */

#include "bench_common.h"

#include <string.h>

static const size_t kSizes[] = {16, 32, 64, 128, 256, 512, 1024, 4096, 16384};
#define NSIZES (sizeof(kSizes) / sizeof(kSizes[0]))

/* Touch one byte per object so the kernel actually commits the page; otherwise
 * we'd be measuring address-space reservation, not allocation. */
static inline void touch(void *p, size_t sz) {
    ((volatile char *)p)[0] = (char)sz;
    if (sz > 64) ((volatile char *)p)[sz - 1] = (char)sz;
}

static void bench_churn(size_t sz, long iters) {
    uint64_t t0 = bench_now_ns();
    for (long i = 0; i < iters; i++) {
        void *p = malloc(sz);
        if (!p) { fprintf(stderr, "OOM churn\n"); exit(1); }
        touch(p, sz);
        free(p);
    }
    uint64_t t1 = bench_now_ns();
    char param[64];
    snprintf(param, sizeof param, "size=%zu", sz);
    bench_emit("micro_churn", param, "ns_per_op", (double)(t1 - t0) / (double)iters);
}

static void bench_batch(size_t sz, long n, const char *order) {
    void **ptrs = bench_meta_alloc((size_t)n * sizeof(void *));
    if (!ptrs) { fprintf(stderr, "meta OOM\n"); exit(1); }

    uint64_t t0 = bench_now_ns();
    for (long i = 0; i < n; i++) {
        ptrs[i] = malloc(sz);
        if (!ptrs[i]) { fprintf(stderr, "OOM batch\n"); exit(1); }
        touch(ptrs[i], sz);
    }
    uint64_t t1 = bench_now_ns();

    /* Build the free order. */
    if (strcmp(order, "lifo") == 0) {
        for (long i = n - 1; i >= 0; i--) free(ptrs[i]);
    } else if (strcmp(order, "fifo") == 0) {
        for (long i = 0; i < n; i++) free(ptrs[i]);
    } else { /* random */
        uint64_t s = 0x9E3779B97F4A7C15ull ^ (uint64_t)sz;
        /* Fisher-Yates shuffle of indices, then free. */
        for (long i = n - 1; i > 0; i--) {
            long j = (long)(bench_rng(&s) % (uint64_t)(i + 1));
            void *tmp = ptrs[i]; ptrs[i] = ptrs[j]; ptrs[j] = tmp;
        }
        for (long i = 0; i < n; i++) free(ptrs[i]);
    }
    uint64_t t2 = bench_now_ns();

    char param[64];
    snprintf(param, sizeof param, "size=%zu", sz);
    char metric[64];
    snprintf(metric, sizeof metric, "alloc_ns_per_op");
    /* alloc cost reported once (same for all orders) under the order tag so the
     * CSV stays one-row-per-(bench,param,metric). */
    char bench[64];
    snprintf(bench, sizeof bench, "micro_%s", order);
    bench_emit(bench, param, "alloc_ns_per_op", (double)(t1 - t0) / (double)n);
    bench_emit(bench, param, "free_ns_per_op", (double)(t2 - t1) / (double)n);

    bench_meta_free(ptrs, (size_t)n * sizeof(void *));
}

int main(int argc, char **argv) {
    long churn_iters = (argc > 1) ? atol(argv[1]) : 5000000;
    long batch_n     = (argc > 2) ? atol(argv[2]) : 1000000;

    for (size_t i = 0; i < NSIZES; i++) {
        size_t sz = kSizes[i];
        /* Fewer iterations for larger sizes to keep wall time bounded. */
        long ci = sz <= 1024 ? churn_iters : churn_iters / 8;
        long bn = sz <= 1024 ? batch_n : batch_n / 8;
        bench_churn(sz, ci);
        bench_batch(sz, bn, "lifo");
        bench_batch(sz, bn, "fifo");
        bench_batch(sz, bn, "random");
    }
    return 0;
}
