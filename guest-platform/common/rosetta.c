#define _GNU_SOURCE
#include "rosetta.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include "np.h"
#include <fcntl.h>
#include <sys/mount.h>
#endif

static const char registration[] =
    ":rosetta:M::\\x7fELF\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00"
    "\\x02\\x00\\x3e\\x00:"
    "\\xff\\xff\\xff\\xff\\xff\\xfe\\xfe\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff"
    "\\xfe\\xff\\xff\\xff:/run/rosetta/rosetta:POCF\n";

const char *np_rosetta_registration(void) { return registration; }

static int has_line(const char *text, const char *line) {
    size_t length = strlen(line);
    for (const char *p = text; *p; ) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == length && !memcmp(p, line, length)) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

int np_rosetta_handler_matches(const char *contents) {
    return contents &&
        has_line(contents, "enabled") &&
        has_line(contents, "interpreter /run/rosetta/rosetta") &&
        has_line(contents, "flags: POCF") && has_line(contents, "offset 0") &&
        has_line(contents, "magic 7f454c4602010100000000000000000002003e00") &&
        has_line(contents, "mask fffffffffffefe00fffffffffffffffffeffffff");
}

int np_rosetta_prepare(void) {
#ifdef __linux__
    if (sysconf(_SC_PAGESIZE) != 4096) {
        fputs("Rosetta requires a 4 KiB ARM64 kernel.\n", stderr);
        errno = ENOTSUP;
        return -1;
    }
    if (np_mkdir_p("/run/rosetta") < 0) return -1;
    if (access("/run/rosetta/rosetta", X_OK) < 0 &&
        mount("rosetta", "/run/rosetta", "virtiofs", MS_RDONLY, NULL) < 0)
        return -1;
    if (access("/run/rosetta/rosetta", X_OK) < 0) return -1;
    const char *directory = "/proc/sys/fs/binfmt_misc";
    const char *control = "/proc/sys/fs/binfmt_misc/register";
    if (np_mkdir_p(directory) < 0) return -1;
    if (access(control, F_OK) < 0 &&
        mount("binfmt_misc", directory, "binfmt_misc", 0, NULL) < 0)
        return -1;
    FILE *existing = fopen("/proc/sys/fs/binfmt_misc/rosetta", "re");
    if (existing) {
        char contents[2048];
        size_t n = fread(contents, 1, sizeof(contents) - 1, existing);
        int error = ferror(existing);
        fclose(existing);
        contents[n] = 0;
        if (!error && np_rosetta_handler_matches(contents)) return 0;
        fputs("An incompatible Rosetta binfmt handler is already registered.\n", stderr);
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) return -1;
    int fd = open(control, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = np_write_full(fd, registration, sizeof(registration) - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
#else
    errno = ENOTSUP;
    return -1;
#endif
}
