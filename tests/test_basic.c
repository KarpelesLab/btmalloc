/*
 * test_basic.c — single-threaded smoke tests for the M0 baseline.
 *
 * Exercises every public API call with simple, deterministic patterns.
 * Each test prints "PASS: <name>" on success and aborts on the first
 * failed assertion. CTest captures the exit status.
 */

#include "btmalloc.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

#define PASS(name) printf("PASS: %s\n", name)

static int is_aligned(const void *p, size_t a) {
    return ((uintptr_t)p & (a - 1)) == 0;
}

static void test_malloc_free_basic(void) {
    void *p = btm_malloc(64);
    CHECK(p != NULL);
    CHECK(is_aligned(p, BTM_ALIGNMENT));
    memset(p, 0xab, 64);
    for (int i = 0; i < 64; ++i) CHECK(((unsigned char *)p)[i] == 0xab);
    btm_free(p);
    PASS("malloc_free_basic");
}

static void test_malloc_zero(void) {
    void *p = btm_malloc(0);
    CHECK(p == NULL); /* M0 contract: 0 yields NULL. Documented. */
    PASS("malloc_zero");
}

static void test_free_null(void) {
    btm_free(NULL); /* must not crash */
    PASS("free_null");
}

static void test_calloc_zeroes(void) {
    void *p = btm_calloc(128, 16);
    CHECK(p != NULL);
    for (int i = 0; i < 128 * 16; ++i) CHECK(((unsigned char *)p)[i] == 0);
    btm_free(p);
    PASS("calloc_zeroes");
}

static void test_calloc_overflow(void) {
    /* Hide the values behind volatile so GCC's alloc_size attribute does
     * not constant-fold and warn about the deliberately-huge product. */
    volatile size_t nmemb = (size_t)-1 / 2;
    volatile size_t sz = 4;
    void *p = btm_calloc(nmemb, sz);
    CHECK(p == NULL);
    PASS("calloc_overflow");
}

static void test_realloc_grow_shrink(void) {
    char *p = btm_malloc(32);
    CHECK(p != NULL);
    memset(p, 0xcd, 32);

    /* Grow */
    char *q = btm_realloc(p, 1024);
    CHECK(q != NULL);
    for (int i = 0; i < 32; ++i) CHECK((unsigned char)q[i] == 0xcd);
    memset(q + 32, 0xef, 1024 - 32);

    /* Shrink (within same M0 mapping is a no-op, but the contract is
     * "old bytes are preserved up to min(old, new)"). */
    char *r = btm_realloc(q, 16);
    CHECK(r != NULL);
    for (int i = 0; i < 16; ++i) CHECK((unsigned char)r[i] == 0xcd);
    btm_free(r);
    PASS("realloc_grow_shrink");
}

static void test_realloc_null(void) {
    /* realloc(NULL, n) is malloc(n). */
    void *p = btm_realloc(NULL, 100);
    CHECK(p != NULL);
    btm_free(p);
    PASS("realloc_null");
}

static void test_realloc_zero(void) {
    /* realloc(p, 0) frees and returns NULL (glibc behavior). */
    void *p = btm_malloc(100);
    CHECK(p != NULL);
    void *q = btm_realloc(p, 0);
    CHECK(q == NULL);
    PASS("realloc_zero");
}

static void test_aligned_alloc(void) {
    for (size_t a = 16; a <= 4096; a <<= 1) {
        size_t sz = a * 2;
        void *p = btm_aligned_alloc(a, sz);
        CHECK(p != NULL);
        CHECK(is_aligned(p, a));
        memset(p, 0x5a, sz);
        btm_free(p);
    }
    PASS("aligned_alloc");
}

static void test_aligned_alloc_invalid(void) {
    errno = 0;
    /* Non-power-of-two alignment. */
    void *p = btm_aligned_alloc(24, 96);
    CHECK(p == NULL);
    CHECK(errno == EINVAL);

    /* size not a multiple of alignment. */
    errno = 0;
    p = btm_aligned_alloc(64, 100);
    CHECK(p == NULL);
    CHECK(errno == EINVAL);
    PASS("aligned_alloc_invalid");
}

static void test_posix_memalign(void) {
    void *p = NULL;
    int rc = btm_posix_memalign(&p, 256, 1000);
    CHECK(rc == 0);
    CHECK(p != NULL);
    CHECK(is_aligned(p, 256));
    memset(p, 0x7e, 1000);
    btm_free(p);

    /* Invalid alignment: not a multiple of sizeof(void *). */
    rc = btm_posix_memalign(&p, 3, 100);
    CHECK(rc == EINVAL);

    /* Invalid alignment: not power of two. */
    rc = btm_posix_memalign(&p, 24, 100);
    CHECK(rc == EINVAL);

    PASS("posix_memalign");
}

static void test_reallocarray_overflow(void) {
    errno = 0;
    volatile size_t nmemb = (size_t)-1 / 4;
    volatile size_t sz = 8;
    void *p = btm_reallocarray(NULL, nmemb, sz);
    CHECK(p == NULL);
    CHECK(errno == ENOMEM);
    PASS("reallocarray_overflow");
}

static void test_usable_size(void) {
    void *p = btm_malloc(100);
    CHECK(p != NULL);
    size_t u = btm_malloc_usable_size(p);
    CHECK(u >= 100);
    btm_free(p);
    CHECK(btm_malloc_usable_size(NULL) == 0);
    PASS("usable_size");
}

static void test_many_small(void) {
    /* Allocate many small blocks, write a tagged pattern, verify, free. */
    enum { N = 256 };
    void *ps[N];
    for (int i = 0; i < N; ++i) {
        size_t sz = (size_t)((i % 64) + 1) * 8;
        ps[i] = btm_malloc(sz);
        CHECK(ps[i] != NULL);
        memset(ps[i], (unsigned char)(i & 0xff), sz);
    }
    for (int i = 0; i < N; ++i) {
        size_t sz = (size_t)((i % 64) + 1) * 8;
        unsigned char exp = (unsigned char)(i & 0xff);
        for (size_t j = 0; j < sz; ++j) {
            CHECK(((unsigned char *)ps[i])[j] == exp);
        }
    }
    for (int i = 0; i < N; ++i) btm_free(ps[i]);
    PASS("many_small");
}

int main(void) {
    test_malloc_free_basic();
    test_malloc_zero();
    test_free_null();
    test_calloc_zeroes();
    test_calloc_overflow();
    test_realloc_grow_shrink();
    test_realloc_null();
    test_realloc_zero();
    test_aligned_alloc();
    test_aligned_alloc_invalid();
    test_posix_memalign();
    test_reallocarray_overflow();
    test_usable_size();
    test_many_small();
    puts("all tests passed");
    return 0;
}
