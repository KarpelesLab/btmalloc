/* prof_churn.c — direct-call churn driver for callgrind attribution.
 * Calls btm_malloc/btm_free directly (no LD_PRELOAD, no valgrind malloc
 * redirection), so callgrind's instruction/cache counts land on our code. */
#include "btmalloc.h"
#include <stdlib.h>

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 2000000;
    int sz = argc > 2 ? atoi(argv[2]) : 64;
    void *sink = 0;
    for (long i = 0; i < n; i++) {
        void *p = btm_malloc((size_t)sz);
        /* touch one byte so the slot isn't optimized away */
        *(volatile char *)p = (char)i;
        sink = p;
        btm_free(p);
    }
    return sink == (void *)1; /* keep sink live */
}
