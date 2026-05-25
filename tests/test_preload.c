/*
 * test_preload.c — validates btmalloc as an LD_PRELOAD drop-in.
 *
 * argv[1] is the path to libbtmalloc.so (passed by CTest). The test:
 *   A. Runs several real programs with LD_PRELOAD set and checks they exit 0
 *      (catches bootstrap/symbol-coverage bugs that unit tests miss).
 *   B. Forks and allocates in the child using the btm_* API directly, checking
 *      the pthread_atfork child handler leaves the allocator usable.
 */

#include "btmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_under_preload(const char *so, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        setenv("LD_PRELOAD", so, 1);
        /* Silence child stdout. */
        if (!freopen("/dev/null", "w", stdout)) _exit(126);
        execvp(argv[0], argv);
        _exit(127); /* exec failed */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int try_prog(const char *so, const char *path, char *const argv[]) {
    if (access(path, X_OK) != 0) {
        fprintf(stderr, "skip: %s not executable\n", path);
        return 0; /* not a failure — just unavailable */
    }
    int rc = run_under_preload(so, argv);
    if (rc != 0) {
        fprintf(stderr, "FAIL: %s under LD_PRELOAD exited %d\n", path, rc);
        return 1;
    }
    printf("PASS: %s under LD_PRELOAD\n", argv[0]);
    return 0;
}

static int test_fork_child_alloc(void) {
    /* Ensure the allocator is initialized (registers the atfork handler). */
    void *warm = btm_malloc(123);
    btm_free(warm);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        /* Child: exercise the allocator after the atfork handler ran. */
        void *ptrs[512];
        for (int i = 0; i < 512; i++) {
            ptrs[i] = btm_malloc((size_t)(i * 7 + 1));
            if (!ptrs[i]) _exit(2);
            memset(ptrs[i], 0xab, (size_t)(i * 7 + 1));
        }
        for (int i = 0; i < 512; i++) btm_free(ptrs[i]);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASS: fork + child allocation\n");
        return 0;
    }
    fprintf(stderr, "FAIL: fork child status %d\n", status);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <libbtmalloc.so>\n", argv[0]); return 2; }
    const char *so = argv[1];
    int fails = 0;

    char *a_true[] = {"/bin/true", NULL};
    char *a_echo[] = {"/bin/echo", "hello", NULL};
    char *a_seq[]  = {"/usr/bin/seq", "1", "5000", NULL};
    char *a_sh[]   = {"/bin/sh", "-c", "ls -la /usr/bin > /dev/null", NULL};

    fails += try_prog(so, "/bin/true", a_true);
    fails += try_prog(so, "/bin/echo", a_echo);
    fails += try_prog(so, "/usr/bin/seq", a_seq);
    fails += try_prog(so, "/bin/sh", a_sh);
    fails += test_fork_child_alloc();

    if (fails) { fprintf(stderr, "test_preload: %d failure(s)\n", fails); return 1; }
    puts("test_preload: all passed");
    return 0;
}
