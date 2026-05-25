/*
 * test_harden.c — freelist safe-linking (security hardening).
 *
 * btmalloc stores each free slot's next-pointer XORed with a per-process secret
 * and the slot's own address. This defeats the classic freelist-corruption
 * primitive (overwrite a freed object's "next" so the allocator hands back an
 * attacker-chosen address): without the secret, an overwrite decodes to an
 * unpredictable pointer the attacker cannot aim.
 *
 * We verify that a freed slot does NOT store the raw address of the next free
 * slot. All allocations come from one call site (a loop body) so they share a
 * partition and thus a single freelist; freeing them in order makes the chain
 * deterministic. (That the encoding still round-trips correctly is exercised
 * exhaustively by test_stress, which runs millions of ops with safe-linking
 * active.)
 */

#include "btmalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do { if (!(e)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e); abort(); } } while (0)

int main(void) {
    const size_t sz = 48;
    enum { N = 8 };
    void *a[N];

    /* One call site (this loop) => one partition => one shared freelist. */
    for (int i = 0; i < N; i++) {
        a[i] = btm_malloc(sz);
        CHECK(a[i]);
        memset(a[i], (unsigned char)(0x40 + i), sz);
    }
    /* Free in order: each free pushes onto the bin head, so the slot just freed
     * links to the one freed before it. After this, a[N-1] is the head and its
     * first word encodes the link to a[N-2]. */
    for (int i = 0; i < N; i++) btm_free(a[i]);

    uintptr_t stored = *(volatile uintptr_t *)a[N - 1];
    /* Stored raw, this would equal a[N-2] exactly — the tcache-poisoning
     * primitive. Safe-linking makes it something unpredictable. */
    CHECK(stored != (uintptr_t)a[N - 2]);
    CHECK(stored != 0);
    printf("PASS: freed slot link is obfuscated (stored=%p != next=%p)\n",
           (void *)stored, a[N - 2]);

    /* Re-allocate and use them to confirm the allocator is still consistent. */
    for (int i = 0; i < N; i++) {
        a[i] = btm_malloc(sz);
        CHECK(a[i]);
        memset(a[i], 0x5a, sz);
    }
    for (int i = 0; i < N; i++) btm_free(a[i]);

    puts("test_harden: all passed");
    return 0;
}
