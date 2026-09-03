#define _GNU_SOURCE
#include "cloud_init_config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int root_path(char *out, size_t cap, const char *root, const char *path) {
    if (!root || !root[0] || strcmp(root, "/") == 0)
        return snprintf(out, cap, "%s", path) < (int)cap ? 0 : -1;
    return snprintf(out, cap, "%s%s", root, path) < (int)cap ? 0 : -1;
}

int np_cloud_init_remove_empty_network_cache(const char *root) {
    char instances[1024];
    if (root_path(instances, sizeof(instances), root, "/var/lib/cloud/instances") < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int root_fd = open(instances, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0)
        return errno == ENOENT ? 0 : -1;
    DIR *dir = fdopendir(root_fd);
    if (!dir) {
        close(root_fd);
        return -1;
    }

    int removed = 0;
    int failed = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        int instance_fd = openat(root_fd, entry->d_name,
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (instance_fd < 0)
            continue;
        struct stat st;
        if (fstatat(instance_fd, "network-config.json", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(st.st_mode) && st.st_size == 0) {
            if (unlinkat(instance_fd, "network-config.json", 0) == 0)
                removed++;
            else
                failed = 1;
        }
        close(instance_fd);
    }
    closedir(dir); /* closes root_fd */
    return failed ? -1 : removed;
}
