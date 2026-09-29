#define _GNU_SOURCE
#include "install.h"
#include "root.h"
#include "os_release.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TRY(call) do { if ((call) != 0) return -1; } while (0)
#define TEXT(root, path, data, mode) np_root_write(root, path, data, strlen(data), mode)

static int restrict_native_apt_sources(int root) {
    char original[16384], updated[32768];
    if (np_root_read(root, "/etc/apt/sources.list.d/ubuntu.sources", original, sizeof(original)) < 0) return -1;
    char *remaining = original, *line;
    size_t offset = 0;
    int stanza = 0, skip = 0;
    while ((line = strsep(&remaining, "\n"))) {
        if (!*line && !remaining) break;
        if (!*line) stanza = 0;
        if (skip && isspace((unsigned char)line[0])) continue;
        skip = !strncasecmp(line, "Architectures:", 14);
        if (skip) continue;
        const char *architecture = "";
        if (!stanza && *line && line[0] != '#') {
            architecture = "Architectures: arm64\n";
            stanza = 1;
        }
        int n = snprintf(updated + offset, sizeof(updated) - offset, "%s%s\n", architecture, line);
        if (n < 0 || n >= (int)(sizeof(updated) - offset)) { errno = E2BIG; return -1; }
        offset += (size_t)n;
    }
    /* Ubuntu 26.04 ARM images use archive.ubuntu.com. Older ones use ports.
     * Preserve the publisher's native mirrors instead of reconstructing them. */
    return TEXT(root, "/etc/apt/sources.list.d/ubuntu.sources", updated, 0644);
}

static int install_wine(struct np_install *install) {
    char data[16384], codename[128], sources[4096];
    if (np_root_read(install->root, "/etc/os-release", data, sizeof(data)) < 0 ||
        np_os_release_value(data, "VERSION_CODENAME", codename, sizeof(codename)) != 1 ||
        !*codename || strspn(codename, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(codename)) {
        errno = EINVAL; return -1;
    }
    TRY(restrict_native_apt_sources(install->root));
    int n = snprintf(sources, sizeof(sources),
        "Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: %s %s-updates %s-backports\n"
        "Components: main universe restricted multiverse\nArchitectures: amd64\n"
        "Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n\n"
        "Types: deb\nURIs: http://security.ubuntu.com/ubuntu/\nSuites: %s-security\n"
        "Components: main universe restricted multiverse\nArchitectures: amd64\n"
        "Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n",
        codename, codename, codename, codename);
    if (n < 0 || n >= (int)sizeof(sources)) return -1;
    TRY(TEXT(install->root, "/etc/apt/sources.list.d/nativepipe-amd64.sources", sources, 0644));
    char *architecture[] = {"/usr/bin/dpkg", "--add-architecture", "amd64", NULL};
    TRY(np_root_run(install->root, architecture, NULL, 0));
    TRY(np_install_package_set(install, 1, ""));
    return np_install_package_set(install, 0, "wine wine64:amd64");
}

static int install_steam(struct np_install *install) {
    TRY(np_install_package_set(install, 0, "curl software-properties-common"));
    char *repository[] = {"/usr/bin/add-apt-repository", "-y", "ppa:fex-emu/fex", NULL};
    TRY(np_root_run(install->root, repository, NULL, 0));
    TRY(np_install_package_set(install, 1, ""));
    /* Explicit FEX launch leaves Rosetta's binfmt handler in control of Wine. */
    TRY(np_install_package_set(install, 0, "fex-emu-armv8.0"));
    char runuser[256], home[128], data[160];
    TRY(np_install_program(install->root, "runuser", runuser));
    snprintf(home, sizeof(home), "HOME=/home/%s", install->username);
    snprintf(data, sizeof(data), "XDG_DATA_HOME=/home/%s/.local/share", install->username);
    char *rootfs[] = {runuser, "-u", install->username, "--", "/usr/bin/env", home, data,
        "FEXRootFSFetcher", "-y", "-a", "--distro-name", "Ubuntu", "--distro-version", "24.04",
        "--distro-list-first", NULL};
    TRY(np_root_run(install->root, rootfs, NULL, 0));
    char *download[] = {"/usr/bin/curl", "--fail", "--location", "--retry", "5",
        "https://repo.steampowered.com/steam/archive/stable/steam-launcher_latest_all.deb",
        "-o", "/var/tmp/nativepipe-steam-launcher.deb", NULL};
    TRY(np_root_run(install->root, download, NULL, 0));
    char *extract[] = {"/usr/bin/dpkg-deb", "--extract", "/var/tmp/nativepipe-steam-launcher.deb", "/", NULL};
    TRY(np_root_run(install->root, extract, NULL, 0));
    TRY(np_root_unlink(install->root, "/var/tmp/nativepipe-steam-launcher.deb"));
    TRY(np_root_unlink(install->root, "/usr/share/applications/steam.desktop"));
    TRY(TEXT(install->root, "/usr/local/bin/lighthouse-steam",
        "#!/bin/sh\nexport STEAMOS=1 STEAM_RUNTIME=1\n"
        "exec FEXBash -c 'exec steam \"$@\"' steam \"$@\"\n", 0755));
    return TEXT(install->root, "/usr/share/applications/lighthouse-steam.desktop",
        "[Desktop Entry]\nName=Steam\nComment=Steam through FEX x86 emulation\n"
        "Exec=/usr/local/bin/lighthouse-steam %U\nIcon=steam\nTerminal=false\n"
        "Type=Application\nCategories=Game;\nMimeType=x-scheme-handler/steam;\n", 0644);
}

int np_install_software(struct np_install *install) {
    if (install->wine) TRY(install_wine(install));
    if (install->steam) TRY(install_steam(install));
    return 0;
}
