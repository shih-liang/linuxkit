#define _GNU_SOURCE
#undef NDEBUG
#include "install.h"
#include "root.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Stand in for a package manager inside the real isolated target process.
 * A package post-install hook replaces DNS, exactly as systemd-resolved does.
 * Every subsequent transaction must still receive usable installation DNS. */
static int transaction(void) {
    assert(getpid() == 1);
    struct stat st;
    assert(lstat("/etc/resolv.conf", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
    int count = 0;
    FILE *f = fopen("/.transactions", "r");
    if (f) { assert(fscanf(f, "%d", &count) == 1); assert(fclose(f) == 0); }
    assert(unlink("/etc/resolv.conf") == 0);
    assert(symlink("/run/not-yet-running-resolver", "/etc/resolv.conf") == 0);
    f = fopen("/.transactions", "w"); assert(f);
    assert(fprintf(f, "%d\n", count + 1) > 0 && fclose(f) == 0);
    return 0;
}

int main(int argc, char **argv) {
    if (!strcmp(argv[0], "/usr/bin/apt-get")) return transaction();
    if (!strcmp(argv[0], "/usr/bin/dpkg")) {
        assert(argc == 3 && !strcmp(argv[1], "--add-architecture") && !strcmp(argv[2], "amd64"));
        return 0;
    }
    if (!strcmp(argv[0], "/usr/bin/pacman-key")) return 0;
    if (!strcmp(argv[0], "/usr/bin/pacman")) {
        if (!strcmp(argv[1], "-R")) {
            assert(argc == 6 && !strcmp(argv[2], "--noconfirm") && !strcmp(argv[3], "--nosave"));
            assert((!strcmp(argv[4], "linux-aarch64") && !strcmp(argv[5], "linux-firmware-nvidia")) ||
                   (!strcmp(argv[5], "linux-aarch64") && !strcmp(argv[4], "linux-firmware-nvidia")));
            int removed = open("/.hardware-removed", O_CREAT | O_WRONLY, 0600);
            assert(removed >= 0); close(removed);
            return 0;
        }
        assert(access("/.hardware-removed", F_OK) == 0);
        return transaction();
    }
    char path[] = "/tmp/nativepipe-package-test.XXXXXX";
    assert(mkdtemp(path));
    int root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); assert(root >= 0);
    assert(np_root_mkdir(root, "/etc", 0755) == 0);
    int self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC); assert(self >= 0);
    assert(np_root_copy(root, "/usr/bin/apt-get", self, 0755) == 0);
    struct np_install install = {.root = root, .distribution = np_distribution_find("debian"), .developer = 1};
    assert(np_install_packages(&install) == 0);
    char count[16];
    assert(np_root_read(root, "/.transactions", count, sizeof(count)) == 2);
    assert(!strcmp(count, "4\n"));
    const char *names[] = {"linux-aarch64", "linux-firmware-nvidia", "linux-api-headers", "mesa", "kmod"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char desc[512], path[512];
        snprintf(desc, sizeof(desc), "%%NAME%%\n%s\n\n%%VERSION%%\n1.0\n", names[i]);
        snprintf(path, sizeof(path), "/var/lib/pacman/local/%s-1.0/desc", names[i]);
        assert(np_root_write(root, path, desc, strlen(desc), 0644) == 0);
    }
    assert(lseek(self, 0, SEEK_SET) == 0);
    assert(np_root_copy(root, "/usr/bin/pacman", self, 0755) == 0);
    assert(lseek(self, 0, SEEK_SET) == 0);
    assert(np_root_copy(root, "/usr/bin/pacman-key", self, 0755) == 0);
    install.distribution = np_distribution_find("archlinux-arm");
    assert(np_install_packages(&install) == 0);
    assert(np_root_read(root, "/.transactions", count, sizeof(count)) == 2 && !strcmp(count, "8\n"));
    assert(np_root_copy(root, "/usr/bin/dpkg", self, 0755) == 0);
    close(self);
    install.distribution = np_distribution_find("ubuntu");
    install.wine = 1;
    const char *mirrors[] = {"http://ports.ubuntu.com/ubuntu-ports/", "http://archive.ubuntu.com/ubuntu/"};
    for (unsigned i = 0; i < 2; i++) {
        const char *release = "ID=ubuntu\nVERSION_CODENAME=resolute\n";
        assert(np_root_write(root, "/etc/os-release", release, strlen(release), 0644) == 0);
        char original[2048], native[4096], translated[4096];
        snprintf(original, sizeof(original),
            "# Publisher configuration\nTypes: deb\nURIs: %s\nSuites: resolute\n"
            "Architectures: arm64\n amd64\n\nTypes: deb\nURIs: %s\nSuites: resolute-security\n",
            mirrors[i], mirrors[i]);
        assert(np_root_write(root, "/etc/apt/sources.list.d/ubuntu.sources", original, strlen(original), 0644) == 0);
        assert(np_install_software(&install) == 0);
        assert(np_root_read(root, "/etc/apt/sources.list.d/ubuntu.sources", native, sizeof(native)) > 0);
        assert(strstr(native, mirrors[i]) && !strstr(native, "amd64"));
        char *first = strstr(native, "Architectures: arm64\n");
        assert(first && strstr(first + 1, "Architectures: arm64\n"));
        assert(np_root_read(root, "/etc/apt/sources.list.d/nativepipe-amd64.sources", translated, sizeof(translated)) > 0);
        assert(strstr(translated, "Architectures: amd64\n") && !strstr(translated, "ports.ubuntu.com"));
    }
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "/var/lib/pacman/local/%s-1.0/desc", names[i]);
        assert(np_root_unlink(root, path) == 0);
        snprintf(path, sizeof(path), "var/lib/pacman/local/%s-1.0", names[i]);
        assert(unlinkat(root, path, AT_REMOVEDIR) == 0);
    }
    assert(np_root_unlink(root, "/usr/bin/pacman") == 0);
    assert(np_root_unlink(root, "/usr/bin/pacman-key") == 0);
    assert(np_root_unlink(root, "/.hardware-removed") == 0);
    assert(np_root_unlink(root, "/usr/bin/apt-get") == 0);
    assert(np_root_unlink(root, "/usr/bin/dpkg") == 0);
    assert(np_root_unlink(root, "/etc/os-release") == 0);
    assert(np_root_unlink(root, "/etc/apt/sources.list.d/ubuntu.sources") == 0);
    assert(np_root_unlink(root, "/etc/apt/sources.list.d/nativepipe-amd64.sources") == 0);
    assert(np_root_unlink(root, "/etc/resolv.conf") == 0);
    assert(np_root_unlink(root, "/.transactions") == 0);
    const char *directories[] = {"usr/bin", "usr", "etc/apt/sources.list.d", "etc/apt", "etc", "dev", "proc", "sys", "run/rosetta", "run",
        "var/lib/pacman/local", "var/lib/pacman", "var/lib", "var"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        int rc = unlinkat(root, directories[i], AT_REMOVEDIR);
        assert(rc == 0 || errno == ENOENT);
    }
    close(root);
    assert(rmdir(path) == 0);
    puts("installer packages: DNS recovery; firmware removal before refresh; preserved native APT mirrors and architecture separation PASS");
    return 0;
}
