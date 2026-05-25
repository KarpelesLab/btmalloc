/*
 * test_tiering.c — live-data cold tiering via btm_pageout_cold().
 *
 * Allocates a large COLD dataset (kept, then left idle) and a small HOT set
 * (kept warm by churn), pages out cold data, and verifies:
 *   - cold objects are still byte-intact after eviction (they fault back from
 *     swap on access) — the safety property;
 *   - RSS drops by roughly the cold footprint (informational; requires swap).
 *
 * The cold and hot sets use distinct call sites (separate loops) and sizes, so
 * they land in different partitions/pools — only the idle cold slabs are
 * evicted.
 */

#include "btmalloc.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(e) do { if (!(e)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e); abort(); } } while (0)

static long rss_kb(void) {
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return -1;
    char b[128];
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    long a = 0, res = 0;
    sscanf(b, "%ld %ld", &a, &res);
    return res * (sysconf(_SC_PAGESIZE) / 1024);
}

int main(void) {
    enum { NCOLD = 120000 };
    const size_t COLD_SZ = 512;  /* ~60 MB of cold data */
    const unsigned char COLD_MAGIC = 0xC0;

    void **cold = calloc(NCOLD, sizeof(void *));
    CHECK(cold);

    /* Allocate and fill the cold set (one call site), then leave it idle. */
    for (int i = 0; i < NCOLD; i++) {
        cold[i] = btm_malloc(COLD_SZ);
        CHECK(cold[i]);
        memset(cold[i], COLD_MAGIC, COLD_SZ);
    }

    /* Baseline pass: establishes the epoch; nothing is idle yet. */
    btm_pageout_cold();

    /* Keep a hot set busy (distinct size/call site) so it stays resident. */
    void *hot[1024];
    for (int i = 0; i < 1024; i++) { hot[i] = btm_malloc(64); CHECK(hot[i]); memset(hot[i], 0x77, 64); }
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < 1024; i++) { btm_free(hot[i]); hot[i] = btm_malloc(64); memset(hot[i], 0x77, 64); }

    long before = rss_kb();
    size_t paged = btm_pageout_cold(); /* cold slabs are now idle => evicted */
    long after = rss_kb();

    printf("tiering: paged out %zu bytes; RSS %ld -> %ld kB (drop %ld)\n",
           paged, before, after, before - after);
    CHECK(paged > 0); /* the cold full slabs should have been evicted */

    /* Safety: every cold object must still read correctly (faults back). */
    long bad = 0;
    for (int i = 0; i < NCOLD; i++) {
        const unsigned char *b = cold[i];
        for (size_t j = 0; j < COLD_SZ; j++) if (b[j] != COLD_MAGIC) { bad++; break; }
    }
    CHECK(bad == 0);
    printf("tiering: all %d cold objects intact after eviction/fault-back\n", NCOLD);

    /* Hot set must be unaffected. */
    for (int i = 0; i < 1024; i++) {
        const unsigned char *b = hot[i];
        for (size_t j = 0; j < 64; j++) CHECK(b[j] == 0x77);
    }

    for (int i = 0; i < NCOLD; i++) btm_free(cold[i]);
    for (int i = 0; i < 1024; i++) btm_free(hot[i]);
    free(cold);

    puts("test_tiering: all passed");
    return 0;
}
