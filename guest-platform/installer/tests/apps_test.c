#define _GNU_SOURCE
#undef NDEBUG
#include "apps.h"
#include "root.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int child(int argc, char **argv) {
    assert(getpid() == 1);
    struct stat st;
    assert(lstat("/etc/resolv.conf", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
    FILE *log = fopen("/operations", "a"); assert(log);
    for (int i = 0; i < argc; i++) fprintf(log, "%s%s", i ? " " : "", argv[i]);
    fputs("\n", log); assert(fclose(log) == 0);
    if (!strcmp(argv[0], "/bin/sh")) {
        assert(argc == 5 && !strcmp(argv[1], "-s") && !strcmp(argv[2], "--"));
        assert(!strcmp(argv[3], "amd64") || !strcmp(argv[3], "arm64"));
        char script[4097]; size_t size = fread(script, 1, sizeof(script)-1, stdin); script[size] = 0;
        assert(strstr(script, "set -eu") && strstr(script, "downloads.claude.ai"));
        if (access("/fail-script", F_OK) == 0) return 29;
    } else if (access("/fail-package", F_OK) == 0) return 23;
    /* A post-install hook breaks DNS; the next preset also needs repair. */
    assert(unlink("/etc/resolv.conf") == 0);
    assert(symlink("/run/no-resolver", "/etc/resolv.conf") == 0);
    return 0;
}

int main(int argc, char **argv) {
    if (!strcmp(argv[0], "/sbin/apk") || !strcmp(argv[0], "/usr/bin/apt-get") ||
        !strcmp(argv[0], "/usr/bin/dnf") || !strcmp(argv[0], "/usr/bin/pacman") || !strcmp(argv[0], "/bin/sh")) return child(argc, argv);
    char folder[] = "/tmp/nativepipe-apps-test.XXXXXX";
    assert(mkdtemp(folder));
    int root = open(folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC); assert(root >= 0);
    assert(np_root_mkdir(root, "/etc", 0755) == 0);
    int self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC); assert(self >= 0);
    const char *binaries[] = {"/sbin/apk", "/usr/bin/apt-get", "/usr/bin/dnf", "/usr/bin/pacman", "/bin/sh"};
    for (size_t i = 0; i < 5; i++) { assert(lseek(self, 0, SEEK_SET) == 0); assert(np_root_copy(root, binaries[i], self, 0755) == 0); }
    close(self);
    const char *policies[] = {"alpine", "debian", "ubuntu", "fedora", "archlinux-arm", "archlinux"};
    const char *names[] = {"python", "claudecode"};
    for (size_t i = 0; i < 6; i++) {
        struct np_install install = {.root = root, .distribution = np_distribution_find(policies[i]), .architecture = i == 4 ? "arm64" : "amd64"};
        assert(np_applications_install(&install, names, 2) == 0);
    }
    char log[32768];
    assert(np_root_read(root, "/operations", log, sizeof(log)) > 0);
    assert(strstr(log, "apk add python3 py3-pip\n"));
    assert(strstr(log, "apt-get install -y --no-install-recommends python3 python3-pip python3-venv\n"));
    assert(strstr(log, "dnf -y --setopt=install_weak_deps=False install python3 python3-pip\n"));
    assert(strstr(log, "pacman -S --noconfirm --needed python python-pip\n"));
    struct np_install install = {.root = root, .distribution = np_distribution_find("alpine"), .architecture = "amd64"};
    assert(np_application_find("claude-code") == np_application_find("claudecode"));
    assert(np_application_find("chatgpt") == np_application_find("chatgpt-desktop"));
    const char *bad[] = {"git", "$(touch /escaped)"};
    assert(np_root_unlink(root, "/operations") == 0);
    assert(np_applications_install(&install, bad, 2) < 0);
    assert(np_root_read(root, "/operations", log, sizeof(log)) < 0 && errno == ENOENT);
    bad[1] = "chatgpt-desktop";
    assert(np_applications_install(&install, bad, 2) < 0);
    bad[0] = "claudecode"; bad[1] = "claude-code";
    assert(np_applications_install(&install, bad, 2) < 0);
    assert(np_root_read(root, "/operations", log, sizeof(log)) < 0 && errno == ENOENT);
    int lock = openat(root, "var/lib/nativepipe-install/applications.lock", O_RDWR | O_CLOEXEC);
    assert(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0);
    assert(np_applications_install(&install, names, 2) < 0);
    close(lock);
    assert(np_root_write(root, "/fail-package", "", 0, 0600) == 0);
    assert(np_applications_install(&install, names, 2) < 0);
    assert(np_root_read(root, "/operations", log, sizeof(log)) > 0 && !strstr(log, "/bin/sh"));
    assert(np_root_unlink(root, "/fail-package") == 0 && np_root_unlink(root, "/operations") == 0);
    assert(np_root_write(root, "/fail-script", "", 0, 0600) == 0);
    assert(np_applications_install(&install, names + 1, 1) < 0);
    assert(np_root_read(root, "/operations", log, sizeof(log)) > 0 && strstr(log, "/bin/sh -s -- amd64 alpine"));
    assert(!strstr(log, "apk add claude-code"));
    for (size_t i = 0; i < 5; i++) assert(np_root_unlink(root, binaries[i]) == 0);
    const char *files[] = {"/operations", "/fail-script", "/etc/resolv.conf", "/var/lib/nativepipe-install/applications.lock"};
    for (size_t i = 0; i < 4; i++) assert(np_root_unlink(root, files[i]) == 0);
    const char *dirs[] = {"sbin", "bin", "usr/bin", "usr", "etc", "dev", "proc", "sys", "run/rosetta", "run", "var/lib/nativepipe-install", "var/lib", "var"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) assert(unlinkat(root, dirs[i], AT_REMOVEDIR) == 0 || errno == ENOENT);
    close(root); assert(rmdir(folder) == 0);
    puts("applications: all package managers, root ABI, DNS, locking, rejection and failure propagation PASS");
    return 0;
}
