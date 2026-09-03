#define _GNU_SOURCE
#include "cloud_init_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "cloud_init_config_test: %s (errno=%d)\n", message, errno);
    exit(1);
}

static void make_dir(const char *path) {
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        fail(path);
}

static void write_file(const char *path, const char *value) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        fail(path);
    size_t len = strlen(value);
    if (len && write(fd, value, len) != (ssize_t)len)
        fail("write");
    if (close(fd) < 0)
        fail("close");
}

int main(void) {
    char root[] = "/tmp/nativepipe-cloud-init-test.XXXXXX";
    if (!mkdtemp(root))
        fail("mkdtemp");

    char path[1024];
    snprintf(path, sizeof(path), "%s/var", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/var/lib", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud/instances", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud/instances/empty", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud/instances/valid", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud/instances/linked", root);
    if (symlink("/tmp", path) < 0)
        fail("symlink");

    char empty[1024], valid[1024];
    snprintf(empty, sizeof(empty),
             "%s/var/lib/cloud/instances/empty/network-config.json", root);
    snprintf(valid, sizeof(valid),
             "%s/var/lib/cloud/instances/valid/network-config.json", root);
    write_file(empty, "");
    write_file(valid, "{}\n");

    int removed = np_cloud_init_remove_empty_network_cache(root);
    if (removed != 1)
        fail("expected one empty cache removal");
    if (access(empty, F_OK) == 0 || errno != ENOENT)
        fail("empty cache still exists");
    if (access(valid, F_OK) != 0)
        fail("valid cache was removed");
    if (np_cloud_init_remove_empty_network_cache(root) != 0)
        fail("repair is not idempotent");

    unlink(valid);
    snprintf(path, sizeof(path), "%s/var/lib/cloud/instances/linked", root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud/instances/empty", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud/instances/valid", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud/instances", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/var/lib/cloud", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/var/lib", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/var", root);
    rmdir(path);
    rmdir(root);

    puts("cloud_init_config_test: ok");
    return 0;
}
