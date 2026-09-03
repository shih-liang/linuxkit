#define _POSIX_C_SOURCE 200809L

#include "console_config.h"

#include <ctype.h>
#include <dirent.h>
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

#define NP_CONSOLE_MAX_FILE (16u * 1024u * 1024u)

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

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int mkdir_parents(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return 0;
    *slash = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int read_file(const char *path, char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0 || (uintmax_t)st.st_size > NP_CONSOLE_MAX_FILE) {
        int saved = errno ? errno : EFBIG;
        close(fd);
        errno = saved;
        return -1;
    }
    size_t cap = (size_t)st.st_size;
    char *mem = malloc(cap + 1);
    if (!mem) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, mem + got, cap - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            free(mem);
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    mem[got] = '\0';
    *out = mem;
    *out_len = got;
    return 0;
}

static int write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
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

static int write_atomic(const char *path, const char *data, size_t len) {
    struct stat st;
    int existed = stat(path, &st) == 0;
    if (!existed && mkdir_parents(path) < 0)
        return -1;

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.nativepipe.XXXXXX", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    mode_t mode = existed ? (st.st_mode & 07777) : 0644;
    int rc = 0;
    if (fchmod(fd, mode) < 0 ||
        (existed && fchown(fd, st.st_uid, st.st_gid) < 0 && errno != EPERM) ||
        write_all(fd, data, len) < 0 || fsync(fd) < 0) {
        rc = -1;
    }
    int saved = errno;
    if (close(fd) < 0 && rc == 0) {
        rc = -1;
        saved = errno;
    }
    if (rc == 0 && rename(tmp, path) < 0) {
        rc = -1;
        saved = errno;
    }
    if (rc != 0)
        unlink(tmp);
    errno = saved;
    return rc;
}

static int token_boundary(unsigned char c) {
    return c == 0 || isspace(c) || c == '\'' || c == '"' || c == '=';
}

static size_t active_line_length(const char *line, size_t len) {
    char quote = 0;
    int escaped = 0;
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (c == '\\' && quote != '\'') {
            escaped = 1;
            continue;
        }
        if (quote) {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '#')
            return i;
    }
    return len;
}

static int span_has_argument(const char *s, size_t len) {
    static const char arg[] = NP_CONSOLE_ARGUMENT;
    const size_t arg_len = sizeof(arg) - 1;
    if (len < arg_len)
        return 0;
    for (size_t i = 0; i + arg_len <= len; i++) {
        if (memcmp(s + i, arg, arg_len) != 0)
            continue;
        unsigned char before = i == 0 ? 0 : (unsigned char)s[i - 1];
        unsigned char after = i + arg_len == len ? 0 : (unsigned char)s[i + arg_len];
        if (token_boundary(before) && token_boundary(after))
            return 1;
    }
    return 0;
}

static int active_line_has_argument(const char *line, size_t len) {
    size_t start = 0;
    while (start < len && isspace((unsigned char)line[start]))
        start++;
    if (start == len || line[start] == '#')
        return 0;
    len = active_line_length(line, len);
    return start < len && span_has_argument(line + start, len - start);
}

static int buffer_has_argument(const char *data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && data[end] != '\n')
            end++;
        if (active_line_has_argument(data + pos, end - pos))
            return 1;
        pos = end < len ? end + 1 : end;
    }
    return 0;
}

static int buffer_assignment_has_argument(const char *data, size_t len,
                                          const char *name) {
    size_t name_len = strlen(name);
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && data[end] != '\n')
            end++;
        size_t start = pos;
        while (start < end && isspace((unsigned char)data[start]))
            start++;
        if (start < end && data[start] != '#' && end - start > name_len &&
            memcmp(data + start, name, name_len) == 0) {
            size_t eq = start + name_len;
            while (eq < end && isspace((unsigned char)data[eq]))
                eq++;
            if (eq < end && data[eq] == '=' &&
                active_line_has_argument(data + start, end - start))
                return 1;
        }
        pos = end < len ? end + 1 : end;
    }
    return 0;
}

int np_console_path_has_hvc0(const char *root, const char *absolute_path) {
    char path[PATH_MAX];
    if (rooted_path(path, sizeof(path), root, absolute_path) < 0)
        return 0;
    char *data = NULL;
    size_t len = 0;
    if (read_file(path, &data, &len) < 0)
        return 0;
    int found = buffer_has_argument(data, len);
    free(data);
    return found;
}

static int append_grub_argument(const char *path, int *changed) {
    static const char block[] =
        "# NativePipe virtio console (managed by nativepipe-guestd)\n"
        "GRUB_CMDLINE_LINUX_DEFAULT=\"${GRUB_CMDLINE_LINUX_DEFAULT} "
        NP_CONSOLE_ARGUMENT "\"\n";
    char *old = NULL;
    size_t old_len = 0;
    if (read_file(path, &old, &old_len) < 0) {
        if (errno != ENOENT)
            return -1;
        old = calloc(1, 1);
        if (!old)
            return -1;
    }
    /* DEFAULT is appended after GRUB_CMDLINE_LINUX in normal menu entries, so
     * hvc0 remains the preferred /dev/console even if a cloud image already
     * carries console=ttyS0 in its base arguments. */
    if (buffer_assignment_has_argument(old, old_len, "GRUB_CMDLINE_LINUX_DEFAULT")) {
        free(old);
        return 0;
    }
    size_t separator = old_len > 0 && old[old_len - 1] != '\n' ? 1 : 0;
    size_t block_len = sizeof(block) - 1;
    char *updated = malloc(old_len + separator + block_len);
    if (!updated) {
        free(old);
        return -1;
    }
    memcpy(updated, old, old_len);
    if (separator)
        updated[old_len] = '\n';
    memcpy(updated + old_len + separator, block, block_len);
    int rc = write_atomic(path, updated, old_len + separator + block_len);
    if (rc == 0)
        *changed = 1;
    free(updated);
    free(old);
    return rc;
}

typedef int (*line_matcher)(const char *, size_t);

static int line_starts_word(const char *line, size_t len, const char *word, int fold_case) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)line[i]))
        i++;
    if (i == len || line[i] == '#')
        return 0;
    size_t word_len = strlen(word);
    if (i + word_len > len)
        return 0;
    for (size_t j = 0; j < word_len; j++) {
        unsigned char a = (unsigned char)line[i + j];
        unsigned char b = (unsigned char)word[j];
        if (fold_case) {
            a = (unsigned char)tolower(a);
            b = (unsigned char)tolower(b);
        }
        if (a != b)
            return 0;
    }
    return i + word_len == len || isspace((unsigned char)line[i + word_len]);
}

static int match_loader_options(const char *line, size_t len) {
    return line_starts_word(line, len, "options", 1);
}

static int match_extlinux_append(const char *line, size_t len) {
    return line_starts_word(line, len, "append", 1);
}

static int match_nonempty(const char *line, size_t len) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)line[i]))
        i++;
    return i < len && line[i] != '#';
}

static int append_argument_to_lines(const char *path, line_matcher matches,
                                    int *configured, int *changed) {
    char *old = NULL;
    size_t old_len = 0;
    if (read_file(path, &old, &old_len) < 0)
        return -1;

    size_t matching = 0;
    size_t missing = 0;
    size_t pos = 0;
    while (pos < old_len) {
        size_t end = pos;
        while (end < old_len && old[end] != '\n')
            end++;
        size_t content_end = end;
        if (content_end > pos && old[content_end - 1] == '\r')
            content_end--;
        if (matches(old + pos, content_end - pos)) {
            matching++;
            if (!active_line_has_argument(old + pos, content_end - pos))
                missing++;
        }
        pos = end < old_len ? end + 1 : end;
    }
    if (matching == 0) {
        free(old);
        return 0;
    }
    *configured = 1;
    if (missing == 0) {
        free(old);
        return 0;
    }

    static const char suffix[] = " " NP_CONSOLE_ARGUMENT;
    size_t suffix_len = sizeof(suffix) - 1;
    if (missing > (SIZE_MAX - old_len) / suffix_len) {
        free(old);
        errno = EOVERFLOW;
        return -1;
    }
    char *updated = malloc(old_len + missing * suffix_len);
    if (!updated) {
        free(old);
        return -1;
    }
    size_t out = 0;
    pos = 0;
    while (pos < old_len) {
        size_t end = pos;
        while (end < old_len && old[end] != '\n')
            end++;
        size_t content_end = end;
        int crlf = content_end > pos && old[content_end - 1] == '\r';
        if (crlf)
            content_end--;
        size_t content_len = content_end - pos;
        memcpy(updated + out, old + pos, content_len);
        out += content_len;
        if (matches(old + pos, content_len) &&
            !active_line_has_argument(old + pos, content_len)) {
            memcpy(updated + out, suffix, suffix_len);
            out += suffix_len;
        }
        if (crlf)
            updated[out++] = '\r';
        if (end < old_len)
            updated[out++] = '\n';
        pos = end < old_len ? end + 1 : end;
    }
    int rc = write_atomic(path, updated, out);
    if (rc == 0)
        *changed = 1;
    free(updated);
    free(old);
    return rc;
}

static int configure_loader_entries(const char *root, struct np_console_result *result) {
    char dir_path[PATH_MAX];
    if (rooted_path(dir_path, sizeof(dir_path), root, "/boot/loader/entries") < 0)
        return -1;
    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }
    result->detected |= NP_CONSOLE_METHOD_LOADER_ENTRY;
    int rc = 0;
    int any_configured = 0;
    int any_changed = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 5, ".conf") != 0)
            continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name) >=
            (int)sizeof(path)) {
            rc = -1;
            errno = ENAMETOOLONG;
            break;
        }
        int configured = 0;
        int changed = 0;
        if (append_argument_to_lines(path, match_loader_options, &configured, &changed) < 0) {
            rc = -1;
            break;
        }
        any_configured |= configured;
        any_changed |= changed;
    }
    int saved = errno;
    closedir(dir);
    errno = saved;
    if (any_configured)
        result->configured |= NP_CONSOLE_METHOD_LOADER_ENTRY;
    if (any_changed)
        result->changed |= NP_CONSOLE_METHOD_LOADER_ENTRY;
    return rc;
}

static int line_has_word(const char *line, size_t len, const char *word) {
    size_t word_len = strlen(word);
    for (size_t i = 0; i + word_len <= len; i++) {
        if (memcmp(line + i, word, word_len) != 0)
            continue;
        int before = i == 0 || !(isalnum((unsigned char)line[i - 1]) || line[i - 1] == '_');
        int after = i + word_len == len ||
                    !(isalnum((unsigned char)line[i + word_len]) || line[i + word_len] == '_');
        if (before && after)
            return 1;
    }
    return 0;
}

static int is_serial_console_name(const char *name) {
    static const char *const prefixes[] = {
        "ttyAMA", "ttyS", "ttyUSB", "ttyACM", "hvc", "xvc", "hvsi",
        "sclp_line", "ttysclp", NULL,
    };
    for (size_t i = 0; prefixes[i]; i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(name, prefixes[i], n) == 0 && name[n] != '\0')
            return 1;
    }
    return 0;
}

/* Find the serial device named by an active getty line.  BusyBox inittab
 * normally repeats it as both the entry id and the final getty argument, while
 * system-V variants may use an arbitrary id and only name it in argv. */
static int getty_serial_device(const char *line, size_t len, char *out, size_t cap) {
    if (!line_has_word(line, len, "getty"))
        return 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && !(isalnum((unsigned char)line[i]) || line[i] == '_'))
            i++;
        size_t start = i;
        while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_' ||
                           line[i] == '-' || line[i] == '.'))
            i++;
        size_t n = i - start;
        if (n == 0 || n >= cap)
            continue;
        memcpy(out, line + start, n);
        out[n] = '\0';
        if (is_serial_console_name(out))
            return 1;
    }
    return 0;
}

static int serial_device_exists(const char *root, const char *name) {
    char absolute[PATH_MAX];
    char resolved[PATH_MAX];
    if (snprintf(absolute, sizeof(absolute), "/dev/%s", name) >= (int)sizeof(absolute))
        return 0;
    if (rooted_path(resolved, sizeof(resolved), root, absolute) < 0)
        return 0;
    return path_exists(resolved);
}

static int ensure_inittab_getty(const char *root, const char *path,
                                int disable_unavailable_gettys,
                                int *configured, int *changed) {
    static const char block[] =
        "# NativePipe virtio console (managed by nativepipe-guestd)\n"
        "hvc0::respawn:/sbin/getty -L 0 hvc0 vt100\n";
    static const char disabled[] =
        "# NativePipe disabled unavailable console: ";
    char *old = NULL;
    size_t old_len = 0;
    if (read_file(path, &old, &old_len) < 0)
        return -1;

    /* Only prune cloud-image UART entries on a live NativePipe guest.  An
     * offline root tree often has no mounted /dev; treating that as evidence
     * that all of its consoles are absent would corrupt a portable image. */
    int prune_unavailable = disable_unavailable_gettys &&
                            serial_device_exists(root, "hvc0");
    size_t disabled_count = 0;
    int has_hvc0 = 0;
    size_t pos = 0;
    while (pos < old_len) {
        size_t end = pos;
        while (end < old_len && old[end] != '\n')
            end++;
        size_t start = pos;
        while (start < end && isspace((unsigned char)old[start]))
            start++;
        if (start < end && old[start] != '#' &&
            line_has_word(old + start, end - start, "getty") &&
            line_has_word(old + start, end - start, "hvc0")) {
            has_hvc0 = 1;
        } else if (prune_unavailable && start < end && old[start] != '#') {
            char device[64];
            if (getty_serial_device(old + start, end - start,
                                    device, sizeof(device)) &&
                strcmp(device, "hvc0") != 0 &&
                !serial_device_exists(root, device)) {
                disabled_count++;
            }
        }
        pos = end < old_len ? end + 1 : end;
    }

    size_t separator = !has_hvc0 && old_len > 0 && old[old_len - 1] != '\n' ? 1 : 0;
    size_t block_len = has_hvc0 ? 0 : sizeof(block) - 1;
    size_t disabled_len = sizeof(disabled) - 1;
    if (disabled_count > (SIZE_MAX - old_len - separator - block_len) / disabled_len) {
        free(old);
        errno = EOVERFLOW;
        return -1;
    }
    size_t updated_len = old_len + disabled_count * disabled_len + separator + block_len;
    if (disabled_count == 0 && has_hvc0) {
        *configured = 1;
        free(old);
        return 0;
    }
    char *updated = malloc(updated_len);
    if (!updated) {
        free(old);
        return -1;
    }
    size_t out_pos = 0;
    pos = 0;
    while (pos < old_len) {
        size_t end = pos;
        while (end < old_len && old[end] != '\n')
            end++;
        size_t start = pos;
        while (start < end && isspace((unsigned char)old[start]))
            start++;
        int disable_line = 0;
        if (prune_unavailable && start < end && old[start] != '#') {
            char device[64];
            disable_line = getty_serial_device(old + start, end - start,
                                                device, sizeof(device)) &&
                           strcmp(device, "hvc0") != 0 &&
                           !serial_device_exists(root, device);
        }
        if (disable_line) {
            memcpy(updated + out_pos, disabled, disabled_len);
            out_pos += disabled_len;
        }
        size_t line_len = end - pos;
        memcpy(updated + out_pos, old + pos, line_len);
        out_pos += line_len;
        if (end < old_len)
            updated[out_pos++] = '\n';
        pos = end < old_len ? end + 1 : end;
    }
    if (separator)
        updated[out_pos++] = '\n';
    if (block_len) {
        memcpy(updated + out_pos, block, block_len);
        out_pos += block_len;
    }
    int rc = write_atomic(path, updated, out_pos);
    if (rc == 0) {
        *configured = 1;
        *changed = 1;
    }
    free(updated);
    free(old);
    return rc;
}

static int configure_single_lines(const char *root, const char *absolute_path,
                                  unsigned method, line_matcher matcher,
                                  struct np_console_result *result) {
    char path[PATH_MAX];
    if (rooted_path(path, sizeof(path), root, absolute_path) < 0)
        return -1;
    if (!path_exists(path))
        return 0;
    result->detected |= method;
    int configured = 0;
    int changed = 0;
    if (append_argument_to_lines(path, matcher, &configured, &changed) < 0)
        return -1;
    if (configured)
        result->configured |= method;
    if (changed)
        result->changed |= method;
    return 0;
}

int np_console_configure_files_selected(const char *root, unsigned methods,
                                        int disable_unavailable_gettys,
                                        struct np_console_result *result) {
    if (!result) {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (!root || !root[0])
        root = "/";

    char grub_default[PATH_MAX];
    char grub_cfg[PATH_MAX];
    char grub2_cfg[PATH_MAX];
    if (rooted_path(grub_default, sizeof(grub_default), root, "/etc/default/grub") < 0 ||
        rooted_path(grub_cfg, sizeof(grub_cfg), root, "/boot/grub/grub.cfg") < 0 ||
        rooted_path(grub2_cfg, sizeof(grub2_cfg), root, "/boot/grub2/grub.cfg") < 0)
        return -1;
    if ((methods & NP_CONSOLE_METHOD_GRUB) &&
        (path_exists(grub_default) || path_exists(grub_cfg) || path_exists(grub2_cfg))) {
        result->detected |= NP_CONSOLE_METHOD_GRUB;
        int changed = 0;
        if (append_grub_argument(grub_default, &changed) < 0)
            return -1;
        result->configured |= NP_CONSOLE_METHOD_GRUB;
        if (changed)
            result->changed |= NP_CONSOLE_METHOD_GRUB;
    }

    if ((methods & NP_CONSOLE_METHOD_LOADER_ENTRY) &&
        configure_loader_entries(root, result) < 0)
        return -1;
    if ((methods & NP_CONSOLE_METHOD_KERNEL_CMDLINE) &&
        configure_single_lines(root, "/etc/kernel/cmdline",
                               NP_CONSOLE_METHOD_KERNEL_CMDLINE, match_nonempty, result) < 0)
        return -1;
    if ((methods & NP_CONSOLE_METHOD_EXTLINUX) &&
        configure_single_lines(root, "/boot/extlinux/extlinux.conf",
                               NP_CONSOLE_METHOD_EXTLINUX, match_extlinux_append, result) < 0)
        return -1;
    if ((methods & NP_CONSOLE_METHOD_EXTLINUX) &&
        configure_single_lines(root, "/boot/syslinux/syslinux.cfg",
                               NP_CONSOLE_METHOD_EXTLINUX, match_extlinux_append, result) < 0)
        return -1;

    char inittab[PATH_MAX];
    if (rooted_path(inittab, sizeof(inittab), root, "/etc/inittab") < 0)
        return -1;
    if ((methods & NP_CONSOLE_METHOD_INITTAB) && path_exists(inittab)) {
        result->detected |= NP_CONSOLE_METHOD_INITTAB;
        int configured = 0;
        int changed = 0;
        if (ensure_inittab_getty(root, inittab, disable_unavailable_gettys,
                                 &configured, &changed) < 0)
            return -1;
        if (configured)
            result->configured |= NP_CONSOLE_METHOD_INITTAB;
        if (changed)
            result->changed |= NP_CONSOLE_METHOD_INITTAB;
    }
    return 0;
}

int np_console_configure_files(const char *root, struct np_console_result *result) {
    const unsigned all = NP_CONSOLE_METHOD_GRUB | NP_CONSOLE_METHOD_LOADER_ENTRY |
                         NP_CONSOLE_METHOD_KERNEL_CMDLINE | NP_CONSOLE_METHOD_EXTLINUX |
                         NP_CONSOLE_METHOD_INITTAB;
    return np_console_configure_files_selected(root, all, 1, result);
}
