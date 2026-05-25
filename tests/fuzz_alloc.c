/*
 * fuzz_alloc.c — operation-sequence fuzzer for btmalloc.
 *
 * Interprets the input bytes as a program of allocator operations
 * (malloc/calloc/realloc/aligned_alloc/free/verify) and runs them against a
 * shadow oracle. Every live allocation gets a unique 32-bit tag written at its
 * start with the rest filled by a derived byte; the tag is re-verified on free,
 * on realloc, and on explicit verify passes. A mismatch means the allocator
 * returned overlapping/aliased memory or corrupted a live object — the bugs
 * that matter most for an allocator. Alignment, usable_size >= requested, and
 * calloc-zeroing are checked inline.
 *
 * Builds two ways from the same code:
 *   - libFuzzer (clang -fsanitize=fuzzer,address, -DBTM_LIBFUZZER): coverage-
 *     guided, LLVMFuzzerTestOneInput drives btmalloc's code paths.
 *   - standalone (any cc): a main() that either replays input files or runs N
 *     PRNG-generated inputs — used as a CI smoke test and a portable fuzzer.
 *
 * The allocator's process-global state persists across inputs (realistic for a
 * long-running program); each input is self-contained and frees everything it
 * allocates, so there are no harness leaks between inputs.
 */

#include "btmalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLIVE 2048

struct slot {
    void    *p;
    size_t   req;     /* requested size */
    uint32_t tag;     /* unique per live allocation */
};

static struct slot live[MAXLIVE];
static int nlive;
static uint32_t tag_ctr;

#define FAIL(...) do { fprintf(stderr, "FUZZ BUG: " __VA_ARGS__); \
    fprintf(stderr, "\n"); abort(); } while (0)

static int is_aligned(const void *p, size_t a) { return ((uintptr_t)p & (a - 1)) == 0; }

/* The byte an allocation with (tag, req) holds at offset i: the 4-byte tag at
 * the front (when it fits), then a tag-derived fill. One source of truth used
 * by stamp, check, and the realloc preserved-prefix check. */
static unsigned char expect_byte(uint32_t tag, size_t req, size_t i) {
    if (req >= 4 && i < 4) return ((const unsigned char *)&tag)[i];
    return (unsigned char)(tag * 31u + 7u);
}

static void stamp(struct slot *s) {
    unsigned char *b = s->p;
    for (size_t i = 0; i < s->req; i++) b[i] = expect_byte(s->tag, s->req, i);
}

static void check(const struct slot *s) {
    const unsigned char *b = s->p;
    for (size_t i = 0; i < s->req; i++)
        if (b[i] != expect_byte(s->tag, s->req, i))
            FAIL("content mismatch at %p+%zu: got %u want %u (overlap/corruption)",
                 s->p, i, b[i], expect_byte(s->tag, s->req, i));
}

/* ---- byte-stream cursor ---- */
static const uint8_t *g_data;
static size_t g_len, g_pos;
static int g_done;

static uint8_t u8(void) {
    if (g_pos >= g_len) { g_done = 1; return 0; }
    return g_data[g_pos++];
}
static uint32_t u16(void) { uint32_t a = u8(); return a | ((uint32_t)u8() << 8); }

static void do_free_at(int i) {
    check(&live[i]);
    btm_free(live[i].p);
    live[i] = live[--nlive]; /* swap-remove */
}

static void run_ops(const uint8_t *data, size_t len) {
    g_data = data; g_len = len; g_pos = 0; g_done = 0;
    nlive = 0;

    while (!g_done) {
        uint8_t op = u8();
        if (g_done) break;
        switch (op % 6) {
        case 0: { /* malloc */
            size_t sz = u16() % 4096 + 1;
            if ((op & 0x40) == 0 && (u8() & 7) == 0) sz = u16() % 200000 + 1; /* occasional large */
            void *p = btm_malloc(sz);
            if (!p) break; /* OOM is allowed */
            if (!is_aligned(p, 16)) FAIL("malloc(%zu) -> %p not 16-aligned", sz, p);
            if (btm_malloc_usable_size(p) < sz) FAIL("usable < requested");
            if (nlive >= MAXLIVE) { btm_free(p); break; }
            live[nlive] = (struct slot){p, sz, ++tag_ctr};
            stamp(&live[nlive]);
            nlive++;
            break;
        }
        case 1: { /* calloc */
            size_t n = (u8() % 64) + 1, e = (u8() % 64) + 1;
            void *p = btm_calloc(n, e);
            if (!p) break;
            size_t sz = n * e;
            for (size_t i = 0; i < sz; i++)
                if (((unsigned char *)p)[i]) FAIL("calloc not zeroed at %zu", i);
            if (nlive >= MAXLIVE) { btm_free(p); break; }
            live[nlive] = (struct slot){p, sz, ++tag_ctr};
            stamp(&live[nlive]);
            nlive++;
            break;
        }
        case 2: { /* realloc */
            if (nlive == 0) break;
            int i = u8() % nlive;
            check(&live[i]);
            size_t ns = u16() % 8192 + 1;
            size_t oreq = live[i].req;
            size_t keep = oreq < ns ? oreq : ns;
            uint32_t tag = live[i].tag;
            void *np = btm_realloc(live[i].p, ns);
            if (!np) break; /* original still valid; leave entry */
            /* Verify the preserved prefix matches the original contents. */
            const unsigned char *b = np;
            for (size_t k = 0; k < keep; k++)
                if (b[k] != expect_byte(tag, oreq, k))
                    FAIL("realloc lost data at %zu (got %u want %u)",
                         k, b[k], expect_byte(tag, oreq, k));
            live[i].p = np; live[i].req = ns;
            stamp(&live[i]); /* re-stamp to the new size */
            break;
        }
        case 3: { /* aligned_alloc */
            size_t align = (size_t)1 << (3 + (u8() % 6)); /* 8..256 */
            size_t sz = ((u16() % 4096) / align + 1) * align; /* multiple of align */
            void *p = btm_aligned_alloc(align, sz);
            if (!p) break;
            if (!is_aligned(p, align)) FAIL("aligned_alloc(%zu) -> %p misaligned", align, p);
            if (nlive >= MAXLIVE) { btm_free(p); break; }
            live[nlive] = (struct slot){p, sz, ++tag_ctr};
            stamp(&live[nlive]);
            nlive++;
            break;
        }
        case 4: { /* free */
            if (nlive == 0) break;
            do_free_at(u8() % nlive);
            break;
        }
        case 5: { /* verify all live */
            for (int i = 0; i < nlive; i++) check(&live[i]);
            break;
        }
        }
        if (nlive >= MAXLIVE) do_free_at(u8() % nlive); /* keep bounded */
    }

    /* Drain: verify and free everything this input allocated. */
    while (nlive) do_free_at(nlive - 1);
}

#ifdef BTM_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    run_ops(data, size);
    return 0;
}
#else
/* Standalone: replay files given as args, else run PRNG-generated inputs.
 *   argv: [iters] [seed]   (defaults 200000, 1) when no files. */
static uint64_t rng(uint64_t *s) {
    uint64_t x = *s; x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x; return x * 0x2545F4914F6CDD1Dull;
}

int main(int argc, char **argv) {
    /* If the first arg names a readable file, treat all args as replay files. */
    FILE *f = (argc > 1) ? fopen(argv[1], "rb") : NULL;
    if (f) {
        for (int i = 1; i < argc; i++) {
            FILE *g = fopen(argv[i], "rb");
            if (!g) continue;
            static uint8_t buf[1 << 20];
            size_t n = fread(buf, 1, sizeof buf, g);
            fclose(g);
            run_ops(buf, n);
            printf("replayed %s (%zu bytes): ok\n", argv[i], n);
        }
        fclose(f);
        return 0;
    }

    long iters = (argc > 1) ? atol(argv[1]) : 200000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 0) : 1;
    uint8_t buf[512];
    for (long it = 0; it < iters; it++) {
        size_t n = 16 + rng(&seed) % (sizeof buf - 16);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rng(&seed);
        run_ops(buf, n);
    }
    printf("fuzz_alloc: %ld random inputs, no bug found\n", iters);
    return 0;
}
#endif
