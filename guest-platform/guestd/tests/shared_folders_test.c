#define _GNU_SOURCE
#include "shared_folders.h"

/* Zig's optimized C builds define NDEBUG; these checks must still execute. */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

static int is_mounted(void) {
    FILE *file = setmntent("/proc/self/mounts", "r");
    assert(file);
    struct mntent *entry;
    int found = 0;
    while ((entry = getmntent(file))) {
        if (strcmp(entry->mnt_dir, NP_HOST_SHARE_MOUNT) == 0)
            found = 1;
    }
    endmntent(file);
    return found;
}

int main(void) {
    /* This test needs root in a Linux VM with the fixed virtiofs device.
     * No mount change can propagate to the VM's actual mount namespace. */
    assert(unshare(CLONE_NEWNS) == 0);
    assert(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
    assert(np_shared_folders_set_mounted(0) == 0);
    assert(!is_mounted());
    assert(np_shared_folders_set_mounted(0) == 0);
    assert(np_shared_folders_set_mounted(1) == 0);
    assert(is_mounted());
    assert(np_shared_folders_set_mounted(1) == 0);
    int fd = open(NP_HOST_SHARE_MOUNT, O_PATH | O_DIRECTORY | O_CLOEXEC);
    assert(fd >= 0);
    errno = 0;
    assert(np_shared_folders_set_mounted(0) < 0 && errno == EBUSY);
    assert(is_mounted());
    close(fd);
    assert(np_shared_folders_set_mounted(0) == 0);
    assert(!is_mounted());
    /* Do not unmount an unrelated filesystem at the managed path. */
    assert(mount("tmpfs", NP_HOST_SHARE_MOUNT, "tmpfs", 0, "size=1m") == 0);
    errno = 0;
    assert(np_shared_folders_set_mounted(0) < 0 && errno == EBUSY);
    assert(np_shared_folders_set_mounted(1) < 0 && errno == EBUSY);
    puts("shared folders: mount, idempotence, busy refusal and namespace isolation passed");
    return 0;
}
