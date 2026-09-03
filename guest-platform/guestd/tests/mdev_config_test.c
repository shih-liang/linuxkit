#include "mdev_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "mdev_config_test: %s\n", message);
    exit(1);
}

static void check(int condition, const char *message) {
    if (!condition)
        fail(message);
}

static void write_fixture(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    check(file != NULL, "could not create fixture");
    check(fputs(text, file) >= 0 && fclose(file) == 0, "could not write fixture");
}

static char *read_fixture(const char *path) {
    FILE *file = fopen(path, "r");
    check(file != NULL, "could not open result");
    check(fseek(file, 0, SEEK_END) == 0, "could not seek result");
    long size = ftell(file);
    check(size >= 0 && fseek(file, 0, SEEK_SET) == 0, "could not size result");
    char *text = calloc((size_t)size + 1, 1);
    check(text != NULL, "allocation failed");
    check(fread(text, 1, (size_t)size, file) == (size_t)size,
          "could not read result");
    fclose(file);
    return text;
}

int main(void) {
    char root[] = "/tmp/nativepipe-mdev-test.XXXXXX";
    check(mkdtemp(root) != NULL, "mkdtemp failed");
    char etc[512], path[512];
    check(snprintf(etc, sizeof(etc), "%s/etc", root) < (int)sizeof(etc),
          "etc path too long");
    check(snprintf(path, sizeof(path), "%s/etc/mdev.conf", root) <
              (int)sizeof(path),
          "mdev path too long");
    check(mkdir(etc, 0755) == 0, "mkdir failed");

    static const char rule[] =
        "SUBSYSTEM=virtio-ports;vport.* root:root 0600 @mkdir -p virtio-ports; "
        "ln -sf ../$MDEV virtio-ports/$(cat /sys/class/virtio-ports/$MDEV/name)\n";
    write_fixture(path,
        "# keep this comment\n"
        "# SUBSYSTEM=virtio-ports;vport.* root:root 0600 @ln -sf ../$MDEV "
        "virtio-ports/$(cat /sys/class/virtio-ports/$MDEV/name)\n"
        "tty[0-9]+ root:tty 0620\n");
    FILE *append = fopen(path, "a");
    check(append != NULL && fputs(rule, append) >= 0 && fclose(append) == 0,
          "could not append rule");

    int changed = 0;
    check(np_mdev_guard_unnamed_virtio_ports(root, &changed) == 0,
          "adapter failed");
    check(changed == 1, "adapter did not report change");
    char *first = read_fixture(path);
    check(strstr(first,
        "[ ! -r /sys/class/virtio-ports/$MDEV/name ] || ln -sf ../$MDEV") != NULL,
        "guard was not installed");
    check(strstr(first, "# keep this comment") != NULL,
          "unrelated content changed");
    check(strstr(first,
        "# SUBSYSTEM=virtio-ports;vport.* root:root 0600 @ln -sf") != NULL,
        "commented rule changed");

    changed = 1;
    check(np_mdev_guard_unnamed_virtio_ports(root, &changed) == 0,
          "idempotent adapter failed");
    check(changed == 0, "idempotent adapter reported a second change");
    char *second = read_fixture(path);
    check(strcmp(first, second) == 0, "idempotent adapter changed the file");
    free(first);
    free(second);

    unlink(path);
    rmdir(etc);
    rmdir(root);
    puts("mdev_config_test: ok");
    return 0;
}
