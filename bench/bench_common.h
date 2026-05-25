/*
 * bench_common.h — shared helpers for btmalloc benchmarks.
 *
 * Benchmarks call the standard malloc/free/realloc so the SAME binary can
 * measure glibc, jemalloc, or btmalloc depending on what is LD_PRELOAD'd.
 * The allocator under test is identified only by the BTM_BENCH_LABEL env var,
 * set by bench/run.sh — the benchmark never links any specific allocator.
 *
 * Output is one CSV row per measurement on stdout:
 *   benchmark,allocator,param,metric,value
 * so results can be concatenated across runs and diffed/plotted.
 */

#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Resident set size in bytes, read from /proc/self/statm without touching the
 * allocator under test (raw syscalls only). Field 2 is resident pages. */
static inline long bench_rss_bytes(void) {
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return -1;
    char buf[128];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    long size_pages = 0, res_pages = 0;
    if (sscanf(buf, "%ld %ld", &size_pages, &res_pages) != 2) return -1;
    return res_pages * sysconf(_SC_PAGESIZE);
}

static inline const char *bench_label(void) {
    const char *l = getenv("BTM_BENCH_LABEL");
    return (l && *l) ? l : "unknown";
}

/* Deterministic, fast PRNG (xorshift64*). */
static inline uint64_t bench_rng(uint64_t *s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* Bookkeeping arrays the benchmark needs (pointer tables, latency samples) are
 * mapped directly so they never perturb the allocator's own statistics. */
#include <sys/mman.h>
static inline void *bench_meta_alloc(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}
static inline void bench_meta_free(void *p, size_t bytes) {
    if (p) munmap(p, bytes);
}

static inline void bench_emit(const char *bench, const char *param,
                              const char *metric, double value) {
    printf("%s,%s,%s,%s,%.4f\n", bench, bench_label(), param, metric, value);
}

#endif /* BENCH_COMMON_H */
