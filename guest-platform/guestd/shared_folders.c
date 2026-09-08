#define _GNU_SOURCE
#include "shared_folders.h"
#include "np.h"

#include <errno.h>
#include <mntent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

static pthread_mutex_t share_lock = PTHREAD_MUTEX_INITIALIZER;

static int mounted_share(void) {
    FILE *file = setmntent("/proc/self/mounts", "r");
    if (!file)
        return -1;
    struct mntent entry;
    char buffer[8192];
    int mounted = 0;
    errno = 0;
    while (getmntent_r(file, &entry, buffer, sizeof(buffer))) {
        if (strcmp(entry.mnt_dir, NP_HOST_SHARE_MOUNT) != 0)
            continue;
        if (strcmp(entry.mnt_type, "virtiofs") != 0 ||
            strcmp(entry.mnt_fsname, NP_HOST_SHARE_TAG) != 0) {
            endmntent(file);
            errno = EBUSY;
            return -1;
        }
        mounted = 1;
    }
    int read_error = ferror(file) || errno == ERANGE;
    endmntent(file);
    if (read_error) { errno = EIO; return -1; }
    return mounted;
}

int np_shared_folders_set_mounted(int desired) {
    pthread_mutex_lock(&share_lock);
    int mounted = mounted_share();
    int result = -1;
    if (mounted < 0)
        goto done;
    if (mounted == !!desired) {
        result = 0;
    } else if (!desired) {
        result = umount(NP_HOST_SHARE_MOUNT);
    } else if (np_mkdir_p(NP_HOST_SHARE_MOUNT) == 0) {
        for (int attempt = 0; attempt < 20; attempt++) {
            result = mount(NP_HOST_SHARE_TAG, NP_HOST_SHARE_MOUNT, "virtiofs",
                           MS_NODEV | MS_NOSUID, NULL);
            if (result == 0 || (errno != ENODEV && errno != ENOENT && errno != EAGAIN))
                break;
            usleep(250000);
        }
    }
done:;
    int saved_errno = errno;
    pthread_mutex_unlock(&share_lock);
    errno = saved_errno;
    return result;
}
