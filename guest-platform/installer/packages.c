#define _GNU_SOURCE
#include "install.h"
#include "root.h"
#include "np_file_rpc.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define TRY(call) do { if ((call) != 0) return -1; } while (0)
#define TEXT(root, path, data, mode) np_root_write(root, path, data, strlen(data), mode)

static int installation_dns(int root) {
    int source = open("/etc/resolv.conf", O_RDONLY | O_CLOEXEC);
    if (source < 0) return -1;
    int rc = np_root_unlink(root, "/etc/resolv.conf");
    if (rc == 0) rc = np_root_copy(root, "/etc/resolv.conf", source, 0644);
    close(source);
    return rc;
}

int np_install_run(struct np_install *install, char *const argv[], const void *input, size_t length) {
    if (!install->live && installation_dns(install->root) < 0) return -1;
    return install->live ? np_root_run_live(argv, input, length)
                         : np_root_run(install->root, argv, input, length);
}

int np_install_package_set(struct np_install *install, int refresh, const char *names) {
    /* Package scripts (notably systemd-resolved) may replace resolv.conf with
     * a link into /run. Each job has a private, empty /run and no running target
     * resolver, so restore installation DNS before every package transaction.
     * np_install_configure installs the final boot-time link afterwards. */
    char *args[128], storage[8192];
    int n = np_package_arguments(install->distribution, refresh, names, args, 126, storage, sizeof(storage));
    if (n < 0) return -1;
    return np_install_run(install, args, NULL, 0);
}

static int remove_unused_hardware(struct np_install *install) {
    if (install->distribution->packages != NP_PACMAN) return 0;
    int fd = np_file_open(install->root, "/var/lib/pacman/local", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return -1;
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return -1; }
    char storage[8192], *next = storage, *args[128] = {
        "/usr/bin/pacman", "-R", "--noconfirm", "--nosave"
    };
    unsigned count = 4;
    int result = -1;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) goto done; break; }
        if (entry->d_name[0] == '.') continue;
        char path[512], data[32768];
        int n = snprintf(path, sizeof(path), "/var/lib/pacman/local/%s/desc", entry->d_name);
        if (n < 0 || n >= (int)sizeof(path)) { errno = ENAMETOOLONG; goto done; }
        if (np_root_read(install->root, path, data, sizeof(data)) < 0) {
            if (errno == ENOTDIR || errno == ENOENT) continue;
            goto done;
        }
        char *name = !strncmp(data, "%NAME%\n", 7) ? data + 7 : strstr(data, "\n%NAME%\n");
        if (!name) { errno = EINVAL; goto done; }
        if (name != data + 7) name += 8;
        char *end = strchr(name, '\n');
        if (!end) { errno = EINVAL; goto done; }
        *end = 0;
        if (!np_distribution_unused_hardware(install->distribution, name)) continue;
        size_t length = strlen(name) + 1;
        if (count + 1 >= sizeof(args) / sizeof(args[0]) ||
            length > sizeof(storage) - (size_t)(next - storage)) { errno = E2BIG; goto done; }
        args[count++] = next;
        memcpy(next, name, length); next += length;
    }
    args[count] = NULL;
    /* Use the package manager before refreshing repositories: no firmware
     * download, no manual package-database edits, and no ignored dependencies.
     * This only operates on a fresh/resumed installer-owned rootfs. */
    result = count == 4 ? 0 : np_root_run(install->root, args, NULL, 0);
done:
    closedir(directory);
    return result;
}

int np_install_packages(struct np_install *install) {
    const struct np_distribution *d = install->distribution;
    TRY(remove_unused_hardware(install));
    if (d->packages == NP_APK) {
        /* The minirootfs may enable main only. Keep its selected stable
         * branch/mirror and enable the matching community repository. */
        char repos[8192];
        if (np_root_read(install->root, "/etc/apk/repositories", repos, sizeof(repos)) < 0) return -1;
        if (!strstr(repos, "/community")) {
            char *main = strstr(repos, "/main");
            if (!main) { errno = EINVAL; return -1; }
            char *start = main;
            while (start > repos && start[-1] != '\n') start--;
            char output[16384];
            int n = snprintf(output, sizeof(output), "%s\n%.*s/community\n", repos, (int)(main - start), start);
            if (n < 0 || n >= (int)sizeof(output)) return -1;
            TRY(TEXT(install->root, "/etc/apk/repositories", output, 0644));
        }
    }
    if (d->packages == NP_PACMAN) {
        if (d->rootfs_format == NP_ROOTFS_ARCH_BOOTSTRAP) {
            /* The official bootstrap ships a commented mirror list. This is
             * a fresh installer-owned root; choose the publisher's HTTPS
             * redirector without changing the ARM publisher's repositories. */
            TRY(TEXT(install->root, "/etc/pacman.d/mirrorlist",
                "Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n", 0644));
        }
        char *init[] = {"/usr/bin/pacman-key", "--init", NULL};
        char *populate[] = {"/usr/bin/pacman-key", "--populate", (char *)d->pacman_keyring, NULL};
        TRY(np_root_run(install->root, init, NULL, 0));
        TRY(np_root_run(install->root, populate, NULL, 0));
    }
    TRY(np_install_package_set(install, 1, ""));
    TRY(np_install_package_set(install, 0, d->base_packages));
    TRY(np_install_package_set(install, 0, d->desktop_packages));
    return 0;
}
