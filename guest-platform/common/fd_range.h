/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/close_range.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Used after fork by guestd and by translated systemd executors. Do not
 * allocate or take stdio locks here. Enumerate sparse open descriptors, not
 * RLIMIT_NOFILE; a missing procfs or a denied syscall must fail closed. */
static int np_close_range(unsigned first, unsigned last, int flags) {
    if (first > last || (flags & ~(CLOSE_RANGE_CLOEXEC | CLOSE_RANGE_UNSHARE))) {
        errno = EINVAL;
        return -1;
    }
    if (syscall(SYS_close_range, first, last, flags) == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return -1;
    if ((flags & CLOSE_RANGE_UNSHARE) && unshare(CLONE_FILES) < 0) return -1;
    struct entry64 {
        uint64_t ino;
        int64_t offset;
        unsigned short length;
        unsigned char type;
        char name[];
    };
    int directory = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -1;
    char data[4096] __attribute__((aligned(8)));
    int result = -1;
    for (;;) {
        long count = syscall(SYS_getdents64, directory, data, sizeof(data));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) break;
        if (!count) { result = 0; break; }
        for (long at = 0; at < count; ) {
            struct entry64 *entry = (struct entry64 *)(data + at);
            size_t prefix = offsetof(struct entry64, name);
            if ((size_t)(count - at) <= prefix || entry->length <= prefix ||
                entry->length > count - at) { errno = EIO; goto done; }
            unsigned fd = 0;
            size_t i = 0, limit = entry->length - prefix;
            for (; i < limit && entry->name[i] >= '0' && entry->name[i] <= '9'; i++) {
                unsigned digit = (unsigned)(entry->name[i] - '0');
                if (fd > ((unsigned)INT_MAX - digit) / 10) { errno = EIO; goto done; }
                fd = fd * 10 + digit;
            }
            if (i == limit) { errno = EIO; goto done; }
            if (i && !entry->name[i] && fd >= first && fd <= last && (int)fd != directory) {
                if (flags & CLOSE_RANGE_CLOEXEC) {
                    int current = fcntl((int)fd, F_GETFD);
                    if (current < 0) { if (errno != EBADF) goto done; }
                    else if (fcntl((int)fd, F_SETFD, current | FD_CLOEXEC) < 0 && errno != EBADF) goto done;
                } else {
                    /* Linux releases the descriptor even if close reports
                     * EINTR or a deferred I/O error. Never retry close. */
                    if (close((int)fd) < 0 && errno != EBADF && errno != EINTR && errno != EIO) goto done;
                }
            }
            at += entry->length;
        }
    }
done:;
    int saved = errno;
    close(directory);
    errno = saved;
    return result;
}
#endif
