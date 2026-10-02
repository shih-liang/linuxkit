#define _GNU_SOURCE
#include "root.h"
#include "np.h"
#include "np_file_rpc.h"
#include "install_error.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int parent_directory(int root, const char *path, char *name, size_t capacity) {
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
    const char *last = strrchr(path, '/');
    if (!last[1] || strlen(last + 1) >= capacity || !strcmp(last + 1, ".") || !strcmp(last + 1, "..")) {
        errno = EINVAL; return -1;
    }
    strcpy(name, last + 1);
    char parent[PATH_MAX];
    size_t n = (size_t)(last - path);
    if (!n) n = 1;
    memcpy(parent, path, n); parent[n] = 0;
    return np_file_open(root, parent, O_RDONLY | O_DIRECTORY, 0);
}

int np_root_mkdir(int root, const char *path, unsigned mode) {
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
    if (!strcmp(path, "/")) return 0;
    char buffer[PATH_MAX];
    strcpy(buffer, path);
    for (char *end = buffer + 1; ; end++) {
        if (*end && *end != '/') continue;
        char saved = *end;
        *end = 0;
        char name[NAME_MAX + 1];
        int parent = parent_directory(root, buffer, name, sizeof(name));
        if (parent < 0) return -1;
        int rc = mkdirat(parent, name, saved ? 0755 : (mode_t)mode);
        int error = errno;
        close(parent);
        if (rc < 0 && error != EEXIST) { errno = error; return -1; }
        int check = np_file_open(root, buffer, O_RDONLY | O_DIRECTORY, 0);
        if (check < 0) return -1;
        close(check);
        *end = saved;
        if (!saved) break;
    }
    return 0;
}

static int make_parents(int root, const char *path) {
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
    char buffer[PATH_MAX];
    strcpy(buffer, path);
    char *last = strrchr(buffer, '/');
    if (last == buffer) return 0;
    *last = 0;
    return np_root_mkdir(root, buffer, 0755);
}

int np_root_write(int root, const char *path, const void *data, size_t length, unsigned mode) {
    if (make_parents(root, path) < 0) return -1;
    int fd = np_file_open(root, path, O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, mode);
    if (fd < 0) return -1;
    struct stat st;
    int rc = fstat(fd, &st);
    if (rc == 0 && !S_ISREG(st.st_mode)) { rc = -1; errno = EINVAL; }
    if (rc == 0) rc = np_write_full(fd, data, length);
    if (rc == 0) rc = fchmod(fd, (mode_t)mode);
    int error = errno;
    if (close(fd) < 0 && rc == 0) return -1;
    errno = error;
    return rc;
}

int np_root_copy(int root, const char *path, int source, unsigned mode) {
    if (make_parents(root, path) < 0) return -1;
    int fd = np_file_open(root, path, O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, mode);
    if (fd < 0) return -1;
    struct stat st;
    int rc = fstat(fd, &st);
    if (rc == 0 && !S_ISREG(st.st_mode)) { rc = -1; errno = EINVAL; }
    char buffer[65536];
    off_t position = 0;
    while (rc == 0) {
        ssize_t n = pread(source, buffer, sizeof(buffer), position);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n < 0) rc = -1; break; }
        if (np_write_full(fd, buffer, (size_t)n) < 0) { rc = -1; break; }
        position += n;
    }
    if (rc == 0) rc = fchmod(fd, (mode_t)mode);
    int error = errno;
    if (close(fd) < 0 && rc == 0) return -1;
    errno = error;
    return rc;
}

ssize_t np_root_read(int root, const char *path, char *data, size_t capacity) {
    if (capacity < 2) { errno = EINVAL; return -1; }
    int fd = np_file_open(root, path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size >= capacity) { close(fd); errno = EFBIG; return -1; }
    size_t n = (size_t)st.st_size;
    int rc = np_read_full(fd, data, n);
    int error = errno;
    close(fd);
    if (rc < 0) { errno = error; return -1; }
    data[n] = 0;
    return (ssize_t)n;
}

int np_root_unlink(int root, const char *path) {
    char name[NAME_MAX + 1];
    int parent = parent_directory(root, path, name, sizeof(name));
    if (parent < 0) return -1;
    int rc = unlinkat(parent, name, 0), error = errno;
    close(parent);
    if (rc < 0 && error != ENOENT) { errno = error; return -1; }
    return 0;
}

int np_root_link(int root, const char *path, const char *target) {
    if (make_parents(root, path) < 0) return -1;
    char name[NAME_MAX + 1];
    int parent = parent_directory(root, path, name, sizeof(name));
    if (parent < 0) return -1;
    int rc = unlinkat(parent, name, 0);
    if (rc == 0 || errno == ENOENT) rc = symlinkat(target, parent, name);
    int error = errno;
    close(parent); errno = error;
    return rc;
}

static int attach(int root, const char *path, const char *source,
                  const char *type, unsigned long flags, const char *data) {
    if (np_root_mkdir(root, path, 0755) < 0) return -1;
    int target = np_file_open(root, path, O_RDONLY | O_DIRECTORY, 0);
    if (target < 0) return -1;
    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", target);
    int rc = mount(source, fdpath, type, flags, data);
    int error = errno;
    close(target); errno = error;
    return rc;
}

static int wait_child(pid_t child) {
    int status;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    if (result < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int np_root_isolate(int root) {
    struct stat original, reopened;
    char fdpath[64], rootpath[PATH_MAX];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", root);
    ssize_t n = readlink(fdpath, rootpath, sizeof(rootpath) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(rootpath) - 1 || fstat(root, &original) < 0) return -1;
    rootpath[n] = 0;
    if (unshare(CLONE_NEWNS) < 0 || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) return -1;
    /* An inherited FD refers to the old namespace's mount. Reopen and check
     * identity before using it as a /proc/self/fd mount destination. */
    int local = open(rootpath, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (local < 0) return -1;
    if (fstat(local, &reopened) < 0 || original.st_dev != reopened.st_dev || original.st_ino != reopened.st_ino) {
        close(local); errno = ESTALE; return -1;
    }
    return local;
}

int np_root_bind(int root, const char *path, const char *source) {
    if (attach(root, path, source, NULL, MS_BIND | MS_REC, NULL) < 0) return -1;
    return attach(root, path, NULL, NULL, MS_SLAVE | MS_REC, NULL);
}

static int run(int root, char *const argv[], const void *input, size_t length) {
    if (!argv || !argv[0] || argv[0][0] != '/' || length > 4096) { errno = EINVAL; return -1; }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) return -1;
    /* Bounded input fits a pipe, so child exit cannot deadlock a writer or
     * deliver SIGPIPE while the parent is trying to report the real error. */
    if (length && np_write_full(pipefd[1], input, length) < 0) {
        close(pipefd[0]); close(pipefd[1]); return -1;
    }
    close(pipefd[1]);
    pid_t owner = getpid(), child = fork();
    if (child < 0) { close(pipefd[0]); return -1; }
    if (child == 0) {
        if (root < 0) {
            if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != owner ||
                dup2(pipefd[0], STDIN_FILENO) < 0 || chdir("/") < 0 || np_child_cloexec() < 0) _exit(126);
            char *const environment[] = {
                "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                "HOME=/root", "TERM=dumb", "LC_ALL=C", "DEBIAN_FRONTEND=noninteractive", NULL,
            };
            execve(argv[0], argv, environment);
            perror(argv[0]); _exit(127);
        }
        int local_root;
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != owner ||
            (local_root = np_root_isolate(root)) < 0 || unshare(CLONE_NEWPID) < 0) {
            perror("nativepipe-install: isolate package operation"); _exit(126);
        }
        close(root);
        root = local_root;
        /* PID 1 sees an external parent as PID 0. A pidfd checks parent
         * liveness across the namespace boundary, including the fork/prctl
         * race, without comparing PIDs from different namespaces. */
        int parent_pidfd = (int)syscall(SYS_pidfd_open, getpid(), 0);
        if (parent_pidfd < 0) _exit(126);
        pid_t init = fork();
        if (init < 0) _exit(126);
        if (init) { close(parent_pidfd); _exit(wait_child(init)); }
        struct pollfd parent_state = {.fd = parent_pidfd, .events = POLLIN};
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || poll(&parent_state, 1, 0) != 0) _exit(126);
        close(parent_pidfd);
        if (attach(root, "/dev", "/dev", NULL, MS_BIND | MS_REC, NULL) < 0 ||
            attach(root, "/proc", "proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0 ||
            attach(root, "/sys", "/sys", NULL, MS_BIND | MS_REC, NULL) < 0 ||
            attach(root, "/run", "tmpfs", "tmpfs", MS_NOSUID | MS_NODEV, "mode=755") < 0 ||
            (access("/run/rosetta/rosetta", X_OK) == 0 &&
             attach(root, "/run/rosetta", "/run/rosetta", NULL, MS_BIND, NULL) < 0)) {
            perror("nativepipe-install: mount package environment"); _exit(126);
        }
        if (dup2(pipefd[0], STDIN_FILENO) < 0 || fchdir(root) < 0 || chroot(".") < 0 || chdir("/") < 0 ||
            np_child_cloexec() < 0) _exit(126);
        char *const environment[] = {
            "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "TERM=dumb", "LC_ALL=C",
            "DEBIAN_FRONTEND=noninteractive", "SYSTEMD_OFFLINE=1", "container=nativepipe-install", NULL,
        };
        execve(argv[0], argv, environment);
        perror(argv[0]); _exit(127);
    }
    close(pipefd[0]);
    int rc = wait_child(child);
    if (rc != 0) {
        char detail[508];
        snprintf(detail, sizeof(detail), "%s exited with status %d. See installation logs for command output", argv[0], rc);
        np_install_report_error(ECHILD, detail);
        fprintf(stderr, "nativepipe-install: %s\n", detail);
        errno = ECHILD;
    }
    return rc;
}

int np_root_run(int root, char *const argv[], const void *input, size_t length) {
    if (root < 0) { errno = EINVAL; return -1; }
    return run(root, argv, input, length);
}

int np_root_run_live(char *const argv[], const void *input, size_t length) {
    return run(-1, argv, input, length);
}
