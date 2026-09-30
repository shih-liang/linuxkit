/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Rosetta returns ENOSYS for faccessat2. libc's older faccessat fallback
 * rejects AT_EMPTY_PATH, which systemd uses to pin its executor. Keep the
 * descriptor pinned and let the real libc/kernel check permissions through
 * procfs; never infer access from mode bits or turn a denial into success.
 * Loaded by translated systemd managers/executors and the explicitly selected
 * systemd services documented in README.md. */
int faccessat(int fd, const char *path, int mode, int flags) {
    int (*original)(int, const char *, int, int) = dlsym(RTLD_NEXT, "faccessat");
    if (!original) { errno = ENOSYS; return -1; }
    int result = original(fd, path, mode, flags);
    if (result == 0 || (errno != EINVAL && errno != ENOSYS) ||
        !(flags & AT_EMPTY_PATH) || (flags & ~(AT_EMPTY_PATH | AT_EACCESS)) ||
        fd < 0 || path[0]) return result;
    int saved = errno;
    struct stat status;
    if (fstat(fd, &status) < 0) return -1;
    /* Following /proc/self/fd for a pinned symlink changes lookup semantics. */
    if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) { errno = saved; return -1; }
    char pinned[64];
    snprintf(pinned, sizeof(pinned), "/proc/self/fd/%d", fd);
    return original(AT_FDCWD, pinned, mode, flags & ~AT_EMPTY_PATH);
}
