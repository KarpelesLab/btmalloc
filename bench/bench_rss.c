/*
 * bench_rss — resident memory and fragmentation over time.
 *
 * Runs a long mixed workload with a realistic churn pattern: a working set of
 * live objects of varied sizes and varied lifetimes. Periodically samples RSS
 * and the number of live bytes the program believes it holds. The ratio
 * RSS / live_bytes is the practical fragmentation+overhead factor — a healthy
 * allocator keeps it low and, crucially, returns memory after a burst.
 *
 * The workload has two phases:
 *   grow   : ramp up to a large live set (peak).
 *   shrink : free most of it, keep churning a small set.
 * A good allocator's RSS should fall during shrink (memory returned to OS),
 * not stay pinned at the peak.
 *
 * Reports CSV rows: phase, op_count, rss_bytes, live_bytes, ratio.
 */

#include "bench_common.h"

#include <string.h>

static const size_t kSizes[] = {16, 32, 48, 64, 96, 128, 192, 256,
                                384, 512, 768, 1024, 2048, 4096, 8192};
#define NSIZES (sizeof(kSizes) / sizeof(kSizes[0]))

struct slot { void *p; size_t sz; };

static void sample(const char *phase, long opc, long live_bytes) {
    long rss = bench_rss_bytes();
    char param[64];
    /* No comma — it would split the CSV's param column. */
    snprintf(param, sizeof param, "%s@%ld", phase, opc);
    bench_emit("rss", param, "rss_bytes", (double)rss);
    bench_emit("rss", param, "live_bytes", (double)live_bytes);
    if (live_bytes > 0)
        bench_emit("rss", param, "ratio", (double)rss / (double)live_bytes);
}

int main(int argc, char **argv) {
    long nslots = (argc > 1) ? atol(argv[1]) : 200000; /* peak live objects */
    long ops    = (argc > 2) ? atol(argv[2]) : 4000000;

    struct slot *live = bench_meta_alloc((size_t)nslots * sizeof(struct slot));
    memset(live, 0, (size_t)nslots * sizeof(struct slot));
    long live_bytes = 0;
    uint64_t s = 0xDEADBEEF12345678ull;

    /* Phase 1: grow — fill the working set. */
    for (long i = 0; i < nslots; i++) {
        size_t sz = kSizes[bench_rng(&s) % NSIZES];
        live[i].p = malloc(sz);
        ((volatile char *)live[i].p)[0] = 1;
        live[i].sz = sz;
        live_bytes += (long)sz;
        if ((i % (nslots / 20 + 1)) == 0) sample("grow", i, live_bytes);
    }
    sample("peak", nslots, live_bytes);

    /* Phase 2: churn — replace random objects, keeping the set roughly full.
     * Exercises in-place reuse and slab recycling. */
    for (long i = 0; i < ops; i++) {
        long idx = (long)(bench_rng(&s) % (uint64_t)nslots);
        if (live[idx].p) { free(live[idx].p); live_bytes -= (long)live[idx].sz; }
        size_t sz = kSizes[bench_rng(&s) % NSIZES];
        live[idx].p = malloc(sz);
        ((volatile char *)live[idx].p)[0] = 1;
        live[idx].sz = sz;
        live_bytes += (long)sz;
        if ((i % (ops / 20 + 1)) == 0) sample("churn", i, live_bytes);
    }

    /* Phase 3: shrink — free 95% and keep churning the rest. RSS should drop. */
    for (long i = 0; i < nslots; i++) {
        if (i % 20 != 0 && live[i].p) {
            free(live[i].p);
            live_bytes -= (long)live[i].sz;
            live[i].p = NULL;
        }
    }
    sample("shrunk", nslots, live_bytes);

    /* Give the allocator a chance to act on a post-burst steady state. */
    for (long i = 0; i < ops / 4; i++) {
        long idx = (long)(bench_rng(&s) % (uint64_t)nslots);
        if (idx % 20 != 0) continue; /* only churn the survivors */
        if (live[idx].p) { free(live[idx].p); live_bytes -= (long)live[idx].sz; }
        size_t sz = kSizes[bench_rng(&s) % NSIZES];
        live[idx].p = malloc(sz);
        ((volatile char *)live[idx].p)[0] = 1;
        live[idx].sz = sz;
        live_bytes += (long)sz;
    }
    sample("steady", nslots, live_bytes);

    for (long i = 0; i < nslots; i++) free(live[i].p);
    sample("drained", 0, 0);

    bench_meta_free(live, (size_t)nslots * sizeof(struct slot));
    return 0;
}
