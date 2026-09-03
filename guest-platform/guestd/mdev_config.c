#define _POSIX_C_SOURCE 200809L

#include "mdev_config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define NP_MDEV_MAX_FILE (1024u * 1024u)

static const char selector[] = "SUBSYSTEM=virtio-ports;vport.*";
static const char unguarded[] =
    "ln -sf ../$MDEV virtio-ports/$(cat /sys/class/virtio-ports/$MDEV/name)";
static const char guard[] =
    "[ ! -r /sys/class/virtio-ports/$MDEV/name ] || ";

static int rooted_path(char *out, size_t cap, const char *root, const char *path) {
    if (!out || cap == 0 || !path || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (!root || !root[0] || strcmp(root, "/") == 0) {
        if (snprintf(out, cap, "%s", path) >= (int)cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/')
        n--;
    if (snprintf(out, cap, "%.*s%s", (int)n, root, path) >= (int)cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int read_file(const char *path, char **out, size_t *out_len,
                     struct stat *metadata) {
    *out = NULL;
    *out_len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 1 : -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > NP_MDEV_MAX_FILE) {
        int saved = errno ? errno : EFBIG;
        close(fd);
        errno = saved;
        return -1;
    }
    size_t cap = (size_t)st.st_size;
    char *data = malloc(cap + 1);
    if (!data) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, data + got, cap - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            int saved = n == 0 ? EIO : errno;
            free(data);
            close(fd);
            errno = saved;
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    data[got] = '\0';
    *out = data;
    *out_len = got;
    if (metadata)
        *metadata = st;
    return 0;
}

static int write_all(int fd, const void *bytes, size_t len) {
    const unsigned char *p = bytes;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_atomic(const char *path, const char *data, size_t len,
                        const struct stat *metadata) {
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.nativepipe.XXXXXX", path) >=
        (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = mkstemp(temporary);
    if (fd < 0)
        return -1;
    int rc = 0;
    if (fchmod(fd, metadata->st_mode & 07777) < 0 ||
        (fchown(fd, metadata->st_uid, metadata->st_gid) < 0 && errno != EPERM) ||
        write_all(fd, data, len) < 0 || fsync(fd) < 0) {
        rc = -1;
    }
    int saved = errno;
    if (close(fd) < 0 && rc == 0) {
        rc = -1;
        saved = errno;
    }
    if (rc == 0 && rename(temporary, path) < 0) {
        rc = -1;
        saved = errno;
    }
    if (rc != 0)
        unlink(temporary);
    errno = saved;
    return rc;
}

static int patchable_line(char *line, char **target) {
    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line || *line == '#')
        return 0;
    if (!strstr(line, selector) || strstr(line, guard))
        return 0;
    *target = strstr(line, unguarded);
    return *target != NULL;
}

int np_mdev_guard_unnamed_virtio_ports(const char *root, int *changed) {
    if (changed)
        *changed = 0;
    char path[PATH_MAX];
    if (rooted_path(path, sizeof(path), root, "/etc/mdev.conf") < 0)
        return -1;

    char *data = NULL;
    size_t len = 0;
    struct stat metadata;
    int read_rc = read_file(path, &data, &len, &metadata);
    if (read_rc == 1)
        return 0;
    if (read_rc < 0)
        return -1;

    size_t patches = 0;
    for (char *line = data; *line;) {
        char *newline = strchr(line, '\n');
        char *end = newline ? newline : data + len;
        char saved = *end;
        *end = '\0';
        char *target = NULL;
        if (patchable_line(line, &target))
            patches++;
        *end = saved;
        line = newline ? newline + 1 : end;
    }
    if (patches == 0) {
        free(data);
        return 0;
    }

    const size_t extra = sizeof(guard) - 1;
    if (patches > (SIZE_MAX - len - 1) / extra) {
        free(data);
        errno = EOVERFLOW;
        return -1;
    }
    char *output = malloc(len + patches * extra + 1);
    if (!output) {
        free(data);
        return -1;
    }
    size_t out_len = 0;
    for (char *line = data; *line;) {
        char *newline = strchr(line, '\n');
        char *end = newline ? newline : data + len;
        char saved = *end;
        *end = '\0';
        char *target = NULL;
        if (patchable_line(line, &target)) {
            size_t prefix = (size_t)(target - line);
            memcpy(output + out_len, line, prefix);
            out_len += prefix;
            memcpy(output + out_len, guard, extra);
            out_len += extra;
            memcpy(output + out_len, target, (size_t)(end - target));
            out_len += (size_t)(end - target);
        } else {
            memcpy(output + out_len, line, (size_t)(end - line));
            out_len += (size_t)(end - line);
        }
        *end = saved;
        if (newline)
            output[out_len++] = '\n';
        line = newline ? newline + 1 : end;
    }
    output[out_len] = '\0';

    int rc = write_atomic(path, output, out_len, &metadata);
    free(output);
    free(data);
    if (rc == 0 && changed)
        *changed = 1;
    return rc;
}
