#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    char path[] = "/tmp/np-faccessat-XXXXXX";
    int writable = mkstemp(path);
    assert(writable >= 0 && fchmod(writable, 0755) == 0);
    int pinned = open(path, O_PATH | O_CLOEXEC);
    assert(pinned >= 0);
    assert(faccessat(pinned, "", X_OK, AT_EMPTY_PATH) == 0);
    assert(faccessat(pinned, "", X_OK, AT_EMPTY_PATH | AT_EACCESS) == 0);
    char link[sizeof(path) + 8];
    assert(snprintf(link, sizeof(link), "%s-link", path) > 0);
    assert(symlink(path, link) == 0);
    int symlink_fd = open(link, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    assert(symlink_fd >= 0);
    assert(faccessat(symlink_fd, "", X_OK, AT_EMPTY_PATH) < 0 && errno == EINVAL);
    close(symlink_fd);
    assert(unlink(link) == 0);
    assert(unlink(path) == 0); /* The original path no longer identifies the file. */
    assert(faccessat(pinned, "", X_OK, AT_EMPTY_PATH) == 0);
    assert(fchmod(writable, 0644) == 0);
    assert(faccessat(pinned, "", X_OK, AT_EMPTY_PATH) < 0 && errno == EACCES);
    assert(faccessat(pinned, "", R_OK, AT_EMPTY_PATH) == 0);
    assert(faccessat(pinned, "", X_OK, AT_EMPTY_PATH | 0x40000000) < 0 && errno == EINVAL);
    close(pinned);
    assert(faccessat(pinned, "", X_OK, AT_EMPTY_PATH) < 0 && errno == EBADF);
    assert(faccessat(AT_FDCWD, "/does-not-exist-nativepipe", X_OK, 0) < 0 && errno == ENOENT);
    close(writable);
    puts("Rosetta access: pinned/unlinked file, real/effective IDs, permission denial and invalid arguments PASS");
    return 0;
}
