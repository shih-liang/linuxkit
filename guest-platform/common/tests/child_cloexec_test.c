#define _GNU_SOURCE
#undef NDEBUG
#include "np.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stddef.h>

/* Exercise the real fallback by making the kernel reject close_range in the
 * child only. All other syscalls and the parent's descriptor table are intact. */
static void reject_close_range(int error) {
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close_range, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (unsigned)error),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {.len = sizeof(code) / sizeof(code[0]), .filter = code};
    assert(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    assert(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0);
}

static void check_case(const char *self, int error) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    struct rlimit limit;
    assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    int minimum = limit.rlim_cur > 8192 ? 8000 : (int)(limit.rlim_cur / 2);
    assert(minimum >= 64);
    int high = fcntl(pipefd[0], F_DUPFD, minimum);
    assert(high >= minimum);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        if (error) reject_close_range(error);
        if (error == EPERM) {
            assert(np_child_cloexec() == -1 && errno == EPERM);
            _exit(0);
        }
        int stdio_flags[3];
        for (int i = 0; i < 3; i++) stdio_flags[i] = fcntl(i, F_GETFD);
        assert(np_child_cloexec() == 0);
        for (int i = 0; i < 3; i++) assert(fcntl(i, F_GETFD) == stdio_flags[i]);
        char first[24], second[24], last[24];
        snprintf(first, sizeof(first), "%d", pipefd[0]);
        snprintf(second, sizeof(second), "%d", pipefd[1]);
        snprintf(last, sizeof(last), "%d", high);
        execl(self, self, "--after-exec", first, second, last, (char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(fcntl(pipefd[0], F_GETFD) == 0 && fcntl(pipefd[1], F_GETFD) == 0);
    assert(fcntl(high, F_GETFD) == 0);
    close(pipefd[0]); close(pipefd[1]); close(high);
}
#endif

int main(int argc, char **argv) {
#ifdef __linux__
    if (argc == 5 && !strcmp(argv[1], "--after-exec")) {
        for (int i = 2; i < 5; i++) {
            errno = 0;
            assert(fcntl(atoi(argv[i]), F_GETFD) == -1 && errno == EBADF);
        }
        return 0;
    }
    check_case(argv[0], 0);
    /* A translated run already exercises ENOSYS but may not implement seccomp.
     * Use --native-only for that runtime; Linux CI covers injected errors. */
    if (argc == 1) {
        check_case(argv[0], ENOSYS);
        check_case(argv[0], EINVAL);
        check_case(argv[0], EOPNOTSUPP);
        check_case(argv[0], EPERM);
    } else assert(argc == 2 && !strcmp(argv[1], "--native-only"));
    puts("child cloexec: stdio, sparse descriptors, exec boundary, parent isolation PASS");
#else
    (void)argc; (void)argv;
    puts("child cloexec: Linux-only test skipped");
#endif
    return 0;
}
