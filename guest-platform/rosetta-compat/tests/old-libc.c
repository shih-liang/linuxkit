#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
int faccessat(int fd, const char *path, int mode, int flags) {
    if (flags & AT_EMPTY_PATH) { errno = EINVAL; return -1; }
    int (*next)(int, const char *, int, int) = dlsym(RTLD_NEXT, "faccessat");
    return next(fd, path, mode, flags);
}
