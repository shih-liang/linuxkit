#define _GNU_SOURCE
#include "apps.h"
#include "root.h"
#include "os_release.h"
#include "np_file_rpc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "applications.inc"

const struct np_application *np_application_find(const char *id) {
    if (!id) return NULL;
    if (!strcmp(id, "claude-code")) id = "claudecode";
    if (!strcmp(id, "chatgpt")) id = "chatgpt-desktop";
    for (size_t i = 0; i < sizeof(applications) / sizeof(applications[0]); i++)
        if (!strcmp(applications[i].id, id)) return &applications[i];
    return NULL;
}

int np_application_supported(const struct np_application *app, const struct np_distribution *d) {
    return app && d && d->packages <= NP_PACMAN &&
        (app->packages[d->packages] || app->scripts[d->packages]);
}

int np_applications_validate(const struct np_install *install, const char *const ids[], size_t count) {
    if (count > NP_MAX_APPLICATIONS || !install->distribution || !install->architecture ||
        (strcmp(install->architecture, "arm64") && strcmp(install->architecture, "amd64"))) {
        errno = EINVAL; return -1;
    }
    for (size_t i = 0; i < count; i++) {
        const struct np_application *app = np_application_find(ids[i]);
        if (!np_application_supported(app, install->distribution)) {
            fprintf(stderr, "nativepipe-install: unsupported application '%s' for %s\n", ids[i], install->distribution->id);
            errno = ENOTSUP; return -1;
        }
        for (size_t j = 0; j < i; j++) {
            if (np_application_find(ids[j]) == app) {
                fprintf(stderr, "nativepipe-install: duplicate application '%s'\n", app->id);
                errno = EINVAL; return -1;
            }
        }
    }
    return 0;
}

int np_applications_install(struct np_install *install, const char *const ids[], size_t count) {
    /* Validate the whole request before the first transaction or repository edit. */
    if (np_applications_validate(install, ids, count) < 0) return -1;
    if (!count) return 0;
    if (np_root_mkdir(install->root, "/var/lib/nativepipe-install", 0700) < 0) return -1;
    int lock = np_file_open(install->root, "/var/lib/nativepipe-install/applications.lock", O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (lock < 0) return -1;
    int result = -1;
    if (flock(lock, LOCK_EX | LOCK_NB) < 0) {
        fputs("nativepipe-install: another application installation is in progress\n", stderr);
        goto done;
    }
    if (np_install_package_set(install, 1, "") != 0) goto done;
    enum np_package_manager manager = install->distribution->packages;
    for (size_t i = 0; i < count; i++) {
        const struct np_application *app = np_application_find(ids[i]);
        fprintf(stderr, "nativepipe-install: application %s\n", app->id);
        if (app->dependencies[manager] && np_install_package_set(install, 0, app->dependencies[manager]) != 0) goto done;
        if (app->scripts[manager]) {
            char *args[] = {"/bin/sh", "-s", "--", (char *)install->architecture, (char *)install->distribution->id, NULL};
            if (np_install_run(install, args, app->scripts[manager], strlen(app->scripts[manager])) != 0) goto done;
            if (app->packages[manager] && np_install_package_set(install, 1, "") != 0) goto done;
        }
        if (app->packages[manager] && np_install_package_set(install, 0, app->packages[manager]) != 0) goto done;
    }
    result = 0;
done:
    close(lock);
    return result;
}

/* Determine the running root's userspace ABI from its ELF, not the host kernel
 * architecture returned by uname under Rosetta. os-release is parsed as data. */
static int detect(struct np_install *install) {
    char release[16384], id[128];
    if (np_root_read(install->root, "/etc/os-release", release, sizeof(release)) < 0 &&
        np_root_read(install->root, "/usr/lib/os-release", release, sizeof(release)) < 0) return -1;
    if (np_os_release_value(release, "ID", id, sizeof(id)) != 1) return -1;
    install->distribution = np_distribution_find(!strcmp(id, "arch") ? "archlinux" :
                                                !strcmp(id, "archarm") ? "archlinux-arm" : id);
    if (!install->distribution) { errno = ENOTSUP; return -1; }
    int fd = np_file_open(install->root, "/bin/sh", O_RDONLY, 0);
    if (fd < 0) return -1;
    unsigned char elf[20];
    ssize_t n = pread(fd, elf, sizeof(elf), 0);
    close(fd);
    if (n != sizeof(elf) || memcmp(elf, "\177ELF", 4) || elf[4] != 2 || elf[5] != 1 || elf[19]) { errno = ENOEXEC; return -1; }
    install->architecture = elf[18] == 183 ? "arm64" : elf[18] == 62 ? "amd64" : NULL;
    if (!install->architecture) { errno = ENOEXEC; return -1; }
    return 0;
}

int np_applications_command(int argc, char **argv) {
    struct np_install install = {.root = -1, .payload = -1, .live = 1};
    int list = argc == 1 && !strcmp(argv[0], "--list");
    if (!argc) {
        fputs("Usage: nativepipe-install apps --list | APP [APP ...]\n", stderr); return 2;
    }
    install.root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (install.root < 0 || detect(&install) < 0) {
        perror("nativepipe-install: unsupported running root");
        if (install.root >= 0) close(install.root);
        return 1;
    }
    int result = 0;
    if (list) {
        for (size_t i = 0; i < sizeof(applications) / sizeof(applications[0]); i++)
            if (np_application_supported(&applications[i], install.distribution))
                printf("%s\t%s\n", applications[i].id, applications[i].name);
    } else if (geteuid() != 0) {
        fputs("nativepipe-install: application installation requires root\n", stderr); result = 1;
    } else result = np_applications_install(&install, (const char *const *)argv, (size_t)argc) != 0;
    close(install.root);
    return result;
}
