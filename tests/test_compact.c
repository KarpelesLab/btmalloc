/*
 * test_compact.c — correctness of Mesh-style compaction (mesh mode).
 *
 * Run with BTM_MESH=1 (set by CTest). Single-threaded, so the heap is quiescent
 * and btm_compact() may safely relocate live objects' physical pages.
 *
 * Strategy: allocate many medium objects (multi-page slabs, the meshable
 * classes), each filled with a per-object magic byte; free most of them so the
 * survivors are sparse across many slabs; compact; then verify EVERY survivor
 * still reads its exact magic end-to-end. A successful mesh remaps a donor
 * slab's virtual pages onto a recipient's physical pages — if the copy/remap is
 * wrong, a survivor's bytes will be corrupted and this catches it.
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

static uint64_t rng(uint64_t *s) {
    uint64_t x = *s; x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x; return x * 0x2545F4914F6CDD1Dull;
}

static long rss_kb(void) {
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return -1;
    char b[128];
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    long sz = 0, res = 0;
    sscanf(b, "%ld %ld", &sz, &res);
    return res * (sysconf(_SC_PAGESIZE) / 1024);
}

struct obj { void *p; size_t sz; uint8_t magic; };

int main(void) {
    /* Medium sizes -> multi-page (meshable) slabs. */
    const size_t sizes[] = {512, 768, 1024, 2048, 4096};
    enum { NS = sizeof(sizes) / sizeof(sizes[0]) };
    enum { N = 120000 };

    struct obj *o = calloc(N, sizeof *o);
    CHECK(o);
    uint64_t s = 0xC0FFEE123;

    for (int i = 0; i < N; i++) {
        size_t sz = sizes[rng(&s) % NS];
        o[i].p = btm_malloc(sz);
        CHECK(o[i].p);
        o[i].sz = sz;
        o[i].magic = (uint8_t)(rng(&s) | 1);
        memset(o[i].p, o[i].magic, sz);
    }

    /* Free ~85% at random, leaving sparse survivors across many slabs. */
    for (int i = 0; i < N; i++) {
        if (rng(&s) % 100 < 85) { btm_free(o[i].p); o[i].p = NULL; }
    }

    long before = rss_kb();
    size_t reclaimed = btm_compact();
    long after = rss_kb();

    /* The whole point: every survivor must still be intact after relocation. */
    long live = 0;
    for (int i = 0; i < N; i++) {
        if (!o[i].p) continue;
        const uint8_t *b = o[i].p;
        for (size_t j = 0; j < o[i].sz; j++) CHECK(b[j] == o[i].magic);
        live++;
    }

    printf("compact: %ld live survivors verified after meshing\n", live);
    printf("compact: reclaimed %zu bytes; RSS %ld -> %ld kB\n",
           reclaimed, before, after);

    /* Churn the survivors a bit post-compact (frees into retired/recipient
     * slabs, reallocs from recipients) and re-verify. */
    for (int i = 0; i < N; i++) {
        if (o[i].p && (rng(&s) & 1)) {
            const uint8_t *b = o[i].p;
            for (size_t j = 0; j < o[i].sz; j++) CHECK(b[j] == o[i].magic);
            btm_free(o[i].p);
            o[i].p = NULL;
        }
    }
    for (int i = 0; i < 20000; i++) {
        size_t sz = sizes[rng(&s) % NS];
        void *p = btm_malloc(sz);
        CHECK(p);
        memset(p, 0x5a, sz);
        btm_free(p);
    }
    for (int i = 0; i < N; i++) {
        if (!o[i].p) continue;
        const uint8_t *b = o[i].p;
        for (size_t j = 0; j < o[i].sz; j++) CHECK(b[j] == o[i].magic);
        btm_free(o[i].p);
    }

    free(o);
    puts("compact: PASS");
    return 0;
}
