/*
 * test_doublefree.c — double-free detection (BTM_HARDEN=1, set by CTest).
 *
 * Verifies that a legitimate alloc/free/realloc workload runs cleanly, and that
 * a genuine double-free is detected and aborts (rather than silently corrupting
 * the heap). The double-free is performed in a forked child so the parent can
 * confirm it died on SIGABRT.
 */

#include "btmalloc.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(e) do { if (!(e)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e); abort(); } } while (0)

#ifndef BTM_HARDENING
#define BTM_HARDENING 1
#endif

int main(void) {
    /* Legitimate churn must not false-positive. */
    void *keep[512];
    for (int i = 0; i < 512; i++) {
        keep[i] = btm_malloc((size_t)(i * 5 + 1));
        CHECK(keep[i]);
        memset(keep[i], 0x33, (size_t)(i * 5 + 1));
    }
    for (int i = 0; i < 512; i++) btm_free(keep[i]);
    /* free / re-alloc / free of the same size class — not a double free. */
    void *a = btm_malloc(64);
    btm_free(a);
    void *b = btm_malloc(64);
    btm_free(b);
    printf("PASS: legitimate workload, no false positive\n");

#if !BTM_HARDENING
    /* Hardening compiled out: no detection to test. */
    puts("test_doublefree: hardening disabled, detection not compiled in (ok)");
    return 0;
#else
    /* Now a real double-free, in a child, expecting SIGABRT. */
    pid_t pid = fork();
    if (pid == 0) {
        /* Silence the abort's diagnostic on stderr for a clean test log. */
        if (!freopen("/dev/null", "w", stderr)) _exit(2);
        void *p = btm_malloc(128);
        btm_free(p);
        btm_free(p); /* should abort here */
        _exit(0);    /* reached only if detection failed */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
    printf("PASS: double-free detected (child aborted on SIGABRT)\n");

    puts("test_doublefree: all passed");
    return 0;
#endif
}
