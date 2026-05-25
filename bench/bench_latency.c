/*
 * bench_latency — per-operation malloc latency distribution.
 *
 * The mean throughput numbers from bench_micro hide the slow-path stalls that
 * matter for latency-critical services: the occasional malloc that triggers an
 * mmap, a page fault storm, or lock contention. This benchmark times each
 * individual malloc, collects the samples, and reports percentiles.
 *
 * The whole point of btmalloc's async backing-store (Phase C) is to flatten
 * the p99/p999 tail by moving mmap/munmap/madvise off the allocating thread,
 * so this benchmark is the primary scoreboard for that work.
 *
 * Timer note: each sample includes ~15-25 ns of CLOCK_MONOTONIC (vDSO)
 * overhead. That biases the median but not the SHAPE of the tail, which is
 * what we track. Lower percentiles are better.
 */

#include "bench_common.h"

#include <string.h>

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void report_pct(uint64_t *samples, long n, const char *param) {
    qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);
    struct { const char *name; double q; } pts[] = {
        {"p50", 0.50}, {"p90", 0.90}, {"p99", 0.99},
        {"p999", 0.999}, {"p9999", 0.9999},
    };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
        long idx = (long)(pts[i].q * (double)(n - 1));
        char metric[32];
        snprintf(metric, sizeof metric, "%s_ns", pts[i].name);
        bench_emit("latency", param, metric, (double)samples[idx]);
    }
    bench_emit("latency", param, "max_ns", (double)samples[n - 1]);
}

int main(int argc, char **argv) {
    long nsamples = (argc > 1) ? atol(argv[1]) : 2000000;
    size_t sz     = (argc > 2) ? (size_t)atol(argv[2]) : 64;
    int    slots  = (argc > 3) ? atoi(argv[3]) : 4096;

    uint64_t *samp = bench_meta_alloc((size_t)nsamples * sizeof(uint64_t));
    void **live = bench_meta_alloc((size_t)slots * sizeof(void *));
    memset(live, 0, (size_t)slots * sizeof(void *));
    uint64_t s = 0xC0FFEEull;

    /* Warm up so steady-state behavior dominates, not first-touch faults. */
    for (int i = 0; i < slots; i++) {
        live[i] = malloc(sz);
        ((volatile char *)live[i])[0] = 1;
    }
    for (int i = 0; i < slots; i++) { free(live[i]); live[i] = NULL; }

    /* Measured loop: each iteration does one timed malloc and one untimed free
     * of an older object, keeping the working set bounded. */
    for (long i = 0; i < nsamples; i++) {
        int idx = (int)(bench_rng(&s) % (uint64_t)slots);
        if (live[idx]) free(live[idx]);
        uint64_t t0 = bench_now_ns();
        void *p = malloc(sz);
        uint64_t t1 = bench_now_ns();
        ((volatile char *)p)[0] = 1;
        live[idx] = p;
        samp[i] = t1 - t0;
    }
    for (int i = 0; i < slots; i++) free(live[i]);

    char param[32];
    snprintf(param, sizeof param, "size=%zu", sz);
    report_pct(samp, nsamples, param);

    bench_meta_free(samp, (size_t)nsamples * sizeof(uint64_t));
    bench_meta_free(live, (size_t)slots * sizeof(void *));
    return 0;
}
