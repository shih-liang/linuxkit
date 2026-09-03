#include "console_config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void fail(const char *message) {
    fprintf(stderr, "console_config_test: %s\n", message);
    exit(1);
}

static void check(int condition, const char *message) {
    if (!condition)
        fail(message);
}

static void make_parents(const char *path) {
    char tmp[PATH_MAX];
    check(snprintf(tmp, sizeof(tmp), "%s", path) < (int)sizeof(tmp), "path too long");
    char *slash = strrchr(tmp, '/');
    if (!slash)
        return;
    *slash = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            fail("mkdir failed");
        *p = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        fail("mkdir failed");
}

static void write_fixture(const char *root, const char *relative,
                          const char *contents, mode_t mode) {
    char path[PATH_MAX];
    check(snprintf(path, sizeof(path), "%s%s", root, relative) < (int)sizeof(path),
          "fixture path too long");
    make_parents(path);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
    check(fd >= 0, "fixture open failed");
    size_t left = strlen(contents);
    const char *p = contents;
    while (left) {
        ssize_t n = write(fd, p, left);
        check(n > 0, "fixture write failed");
        p += n;
        left -= (size_t)n;
    }
    check(close(fd) == 0, "fixture close failed");
    check(chmod(path, mode) == 0, "fixture chmod failed");
}

static char *read_fixture(const char *root, const char *relative) {
    char path[PATH_MAX];
    check(snprintf(path, sizeof(path), "%s%s", root, relative) < (int)sizeof(path),
          "read path too long");
    FILE *f = fopen(path, "rb");
    check(f != NULL, "fixture read failed");
    check(fseek(f, 0, SEEK_END) == 0, "fixture seek failed");
    long end = ftell(f);
    check(end >= 0 && fseek(f, 0, SEEK_SET) == 0, "fixture tell failed");
    char *data = malloc((size_t)end + 1);
    check(data != NULL, "fixture allocation failed");
    check(fread(data, 1, (size_t)end, f) == (size_t)end, "fixture fread failed");
    data[end] = '\0';
    fclose(f);
    return data;
}

static size_t occurrences(const char *haystack, const char *needle) {
    size_t count = 0;
    size_t n = strlen(needle);
    for (const char *p = haystack; (p = strstr(p, needle)) != NULL; p += n)
        count++;
    return count;
}

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        unlink(path);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            (int)sizeof(child))
            fail("cleanup path too long");
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(child);
        else
            unlink(child);
    }
    closedir(dir);
    rmdir(path);
}

int main(void) {
    char root_template[] = "/tmp/nativepipe-console-test.XXXXXX";
    char *root = mkdtemp(root_template);
    check(root != NULL, "mkdtemp failed");

    write_fixture(root, "/etc/default/grub",
                  "# an inactive console=hvc0 must not suppress provisioning\n"
                  "GRUB_TIMEOUT=0\n"
                  "GRUB_CMDLINE_LINUX=\"root=UUID=test panic=30\"\n",
                  0600);
    write_fixture(root, "/boot/loader/entries/linux.conf",
                  "title Test Linux\noptions root=UUID=test ro quiet\n", 0644);
    write_fixture(root, "/boot/loader/entries/already.conf",
                  "title Existing\noptions root=UUID=test console=hvc0\n", 0644);
    write_fixture(root, "/etc/kernel/cmdline", "root=UUID=test ro\n", 0644);
    write_fixture(root, "/boot/extlinux/extlinux.conf",
                  "DEFAULT linux\nLABEL linux\n  APPEND root=/dev/vda2 quiet\n"
                  "# APPEND console=hvc0\n", 0644);
    write_fixture(root, "/etc/inittab",
                  "::sysinit:/sbin/openrc sysinit\n"
                  "tty1::respawn:/sbin/getty 38400 tty1\n"
                  "ttyAMA0::respawn:/sbin/getty -L 115200 ttyAMA0 vt100\n", 0644);
    /* A mounted /dev/hvc0 opts into live-guest cleanup. tty1 is present and
     * must remain active; ttyAMA0 is deliberately absent. */
    write_fixture(root, "/dev/hvc0", "", 0600);
    write_fixture(root, "/dev/tty1", "", 0600);
    write_fixture(root, "/proc/cmdline", "root=/dev/vda2 ro\n", 0444);

    struct np_console_result first;
    check(np_console_configure_files(root, &first) == 0, "first configure failed");
    unsigned expected = NP_CONSOLE_METHOD_GRUB | NP_CONSOLE_METHOD_LOADER_ENTRY |
                        NP_CONSOLE_METHOD_KERNEL_CMDLINE | NP_CONSOLE_METHOD_EXTLINUX |
                        NP_CONSOLE_METHOD_INITTAB;
    check((first.detected & expected) == expected, "not all fixture methods detected");
    check((first.configured & expected) == expected, "not all fixture methods configured");
    check((first.changed & expected) == expected, "not all fixture methods changed");

    char *grub = read_fixture(root, "/etc/default/grub");
    check(strstr(grub, "root=UUID=test panic=30") != NULL, "GRUB arguments lost");
    check(strstr(grub, "${GRUB_CMDLINE_LINUX_DEFAULT} console=hvc0") != NULL,
          "GRUB managed assignment missing");
    check(strstr(grub, "console=ttyS0") == NULL, "unavailable UART console added");
    check(occurrences(grub, "GRUB_CMDLINE_LINUX=") == 1, "unexpected GRUB rewrite");
    free(grub);

    char *loader = read_fixture(root, "/boot/loader/entries/linux.conf");
    check(strstr(loader, "options root=UUID=test ro quiet console=hvc0") != NULL,
          "loader options not updated");
    check(occurrences(loader, "console=hvc0") == 1, "loader argument duplicated");
    free(loader);
    char *already = read_fixture(root, "/boot/loader/entries/already.conf");
    check(occurrences(already, "console=hvc0") == 1, "existing loader argument duplicated");
    free(already);

    char *kernel = read_fixture(root, "/etc/kernel/cmdline");
    check(strcmp(kernel, "root=UUID=test ro console=hvc0\n") == 0,
          "kernel cmdline not preserved");
    free(kernel);
    char *extlinux = read_fixture(root, "/boot/extlinux/extlinux.conf");
    check(strstr(extlinux, "APPEND root=/dev/vda2 quiet console=hvc0") != NULL,
          "extlinux APPEND not updated");
    check(occurrences(extlinux, "console=hvc0") == 2,
          "comment and active extlinux arguments should each occur once");
    free(extlinux);
    char *inittab = read_fixture(root, "/etc/inittab");
    check(strstr(inittab, "hvc0::respawn:/sbin/getty -L 0 hvc0 vt100") != NULL,
          "OpenRC hvc0 getty missing");
    check(strstr(inittab, "tty1::respawn:/sbin/getty 38400 tty1") != NULL,
          "available virtual console was disabled");
    check(strstr(inittab,
                 "# NativePipe disabled unavailable console: ttyAMA0::respawn:") != NULL,
          "unavailable cloud UART getty was not disabled");
    free(inittab);

    char grub_path[PATH_MAX];
    check(snprintf(grub_path, sizeof(grub_path), "%s/etc/default/grub", root) <
              (int)sizeof(grub_path),
          "mode path too long");
    struct stat grub_stat;
    check(stat(grub_path, &grub_stat) == 0 && (grub_stat.st_mode & 0777) == 0600,
          "atomic update did not preserve mode");
    check(np_console_path_has_hvc0(root, "/proc/cmdline") == 0,
          "inactive current kernel reported configured");

    struct np_console_result second;
    check(np_console_configure_files(root, &second) == 0, "second configure failed");
    check(second.changed == 0, "second configure was not idempotent");

    remove_tree(root);

    char selected_template[] = "/tmp/nativepipe-console-selected-test.XXXXXX";
    char *selected_root = mkdtemp(selected_template);
    check(selected_root != NULL, "selected mkdtemp failed");
    write_fixture(selected_root, "/etc/default/grub", "GRUB_TIMEOUT=0\n", 0644);
    write_fixture(selected_root, "/etc/inittab",
                  "ttyAMA0::respawn:/sbin/getty -L 115200 ttyAMA0 vt100\n", 0644);
    write_fixture(selected_root, "/dev/hvc0", "", 0600);
    struct np_console_result selected;
    check(np_console_configure_files_selected(
              selected_root, NP_CONSOLE_METHOD_GRUB, 0, &selected) == 0,
          "selected configure failed");
    check(selected.detected == NP_CONSOLE_METHOD_GRUB,
          "unselected adapter was detected");
    inittab = read_fixture(selected_root, "/etc/inittab");
    check(strcmp(inittab,
                 "ttyAMA0::respawn:/sbin/getty -L 115200 ttyAMA0 vt100\n") == 0,
          "systemd-style profile unexpectedly rewrote inittab");
    free(inittab);
    remove_tree(selected_root);

    puts("console_config_test: ok");
    return 0;
}
