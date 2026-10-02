#define _GNU_SOURCE
#include "install.h"
#include "root.h"
#include "np_file_rpc.h"
#include "rosetta.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TRY(call) do { if ((call) != 0) return -1; } while (0)
#define TEXT(root, path, data, mode) np_root_write(root, path, data, strlen(data), mode)

int np_install_program(int root, const char *name, char path[256]) {
    static const char *directories[] = {"/usr/sbin", "/usr/bin", "/sbin", "/bin"};
    if (strchr(name, '/')) { errno = EINVAL; return -1; }
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        int n = snprintf(path, 256, "%s/%s", directories[i], name);
        if (n < 0 || n >= 256) { errno = ENAMETOOLONG; return -1; }
        int fd = np_file_open(root, path, O_RDONLY | O_NONBLOCK, 0);
        if (fd < 0) continue;
        struct stat st;
        int executable = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
        close(fd);
        if (executable) return 0;
    }
    errno = ENOENT;
    return -1;
}

/* Work with the target account files, not libc's native account database. */
static int account_fields(char *line, char *fields[7]) {
    for (unsigned i = 0; i < 7; i++) {
        fields[i] = strsep(&line, ":");
        if (!fields[i]) return -1;
    }
    return line ? -1 : 0;
}

int np_install_account(struct np_install *install) {
    char entries[131072], *save = NULL, *line, *fields[7];
    if (np_root_read(install->root, "/etc/passwd", entries, sizeof(entries)) < 0) return -1;
    int found = 0;
    for (line = strtok_r(entries, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (account_fields(line, fields) < 0) return -1;
        if (strcmp(fields[0], install->username)) continue;
        char *end;
        unsigned long uid = strtoul(fields[2], &end, 10);
        if (*end || uid < 1000 || uid >= 65534 || (install->uid && uid != install->uid)) {
            errno = EACCES; return -1;
        }
        install->uid = (uid_t)uid;
        install->gid = (gid_t)strtoul(fields[3], &end, 10);
        if (*end) return -1;
        found = 1;
    }
    char program[256];
    if (!found) {
        TRY(np_install_program(install->root, "useradd", program));
        char *create[] = {program, "-m", "-U", "-s", (char *)install->distribution->shell, install->username, NULL};
        TRY(np_root_run(install->root, create, NULL, 0));
        if (np_root_read(install->root, "/etc/passwd", entries, sizeof(entries)) < 0) return -1;
        for (line = strtok_r(entries, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (account_fields(line, fields) < 0) return -1;
            if (strcmp(fields[0], install->username)) continue;
            install->uid = (uid_t)strtoul(fields[2], NULL, 10);
            install->gid = (gid_t)strtoul(fields[3], NULL, 10);
            found = 1;
            break;
        }
        if (!found || install->uid < 1000 || install->uid >= 65534) return -1;
    }
    TRY(np_install_program(install->root, "usermod", program));
    if (np_root_read(install->root, "/etc/passwd", entries, sizeof(entries)) < 0) return -1;
    for (line = strtok_r(entries, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (account_fields(line, fields) < 0) return -1;
        unsigned long uid = strtoul(fields[2], NULL, 10);
        if (!strcmp(fields[0], install->username) || (uid != 0 && (uid < 1000 || uid >= 65534))) continue;
        char *lock[] = {program, "-L", fields[0], NULL};
        TRY(np_root_run(install->root, lock, NULL, 0));
    }
    if (np_root_read(install->root, "/etc/group", entries, sizeof(entries)) < 0) return -1;
    char groups[256] = "";
    for (line = strtok_r(entries, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        if (strcmp(line, "sudo") && strcmp(line, "wheel") && strcmp(line, "audio") &&
            strcmp(line, "video") && strcmp(line, "render") && strcmp(line, "input") &&
            (strcmp(line, "realtime") || install->distribution->packages != NP_PACMAN)) continue;
        if (strlen(groups) + strlen(line) + 2 >= sizeof(groups)) return -1;
        if (*groups) strcat(groups, ",");
        strcat(groups, line);
    }
    if (*groups) {
        char *add[] = {program, "-aG", groups, install->username, NULL};
        TRY(np_root_run(install->root, add, NULL, 0));
    }
    TRY(np_install_program(install->root, "chpasswd", program));
    char secret[sizeof(install->password) + sizeof(install->username) + 3];
    int n = snprintf(secret, sizeof(secret), "%s:%s\n", install->username, install->password);
    if (n < 0 || n >= (int)sizeof(secret)) return -1;
    char *password[] = {program, NULL};
    int rc = np_root_run(install->root, password, secret, (size_t)n);
    explicit_bzero(secret, sizeof(secret));
    if (rc != 0) return -1;
    char sudoers[128];
    snprintf(sudoers, sizeof(sudoers), "%s ALL=(ALL:ALL) ALL\n", install->username);
    TRY(TEXT(install->root, "/etc/sudoers.d/90-linportal-user", sudoers, 0440));
    return 0;
}

static int systemd_enable(int root, const char *target, const char *unit, const char *file) {
    const char *directories[] = {"/etc/systemd/system", "/usr/lib/systemd/system", "/lib/systemd/system"};
    char source[512], destination[512];
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        snprintf(source, sizeof(source), "%s/%s", directories[i], file ? file : unit);
        int fd = np_file_open(root, source, O_RDONLY, 0);
        if (fd < 0) continue;
        close(fd);
        snprintf(destination, sizeof(destination), "/etc/systemd/system/%s.wants/%s", target, unit);
        return np_root_link(root, destination, source);
    }
    fprintf(stderr, "nativepipe-install: missing systemd unit %s\n", unit);
    return -1;
}

static int openrc_enable(int root, const char *level, const char *service) {
    char source[256], destination[256];
    snprintf(source, sizeof(source), "/etc/init.d/%s", service);
    int fd = np_file_open(root, source, O_RDONLY, 0);
    if (fd < 0) return -1;
    close(fd);
    snprintf(destination, sizeof(destination), "/etc/runlevels/%s/%s", level, service);
    return np_root_link(root, destination, source);
}

int np_install_configure(struct np_install *install) {
    int root = install->root;
    TRY(TEXT(root, "/etc/hostname", "linportal\n", 0644));
    TRY(TEXT(root, "/etc/fstab", "LABEL=nativepipe-root / ext4 defaults 0 1\n", 0644));
    /* Never retain an image publisher's machine identity. systemd initializes
     * an empty file on first boot; OpenRC's dbus rejects an existing empty file
     * and needs its normal UUID generator before services can start. */
    TRY(np_root_unlink(root, "/var/lib/dbus/machine-id"));
    TRY(np_root_unlink(root, "/etc/machine-id"));
    if (install->distribution->init == NP_SYSTEMD) {
        if (install->rosetta && !strcmp(install->architecture, "amd64")) {
            /* systemd's executor inherits its manager's environment. Unit
             * payloads have a separate environment, so the library stays in
             * the managers/executors rather than every Linux application. */
            int library = open(NP_ROSETTA_COMPAT_PATH, O_RDONLY | O_CLOEXEC);
            if (library < 0) return -1;
            int copied = np_root_copy(root, NP_ROSETTA_COMPAT_PATH, library, 0755);
            close(library);
            TRY(copied);
            TRY(TEXT(root, "/etc/systemd/system/user@.service.d/50-nativepipe-rosetta.conf",
                "[Service]\nEnvironment=LD_PRELOAD=" NP_ROSETTA_COMPAT_PATH "\n", 0644));
            TRY(TEXT(root, "/etc/systemd/user.conf.d/50-nativepipe-rosetta.conf",
                "[Manager]\nDefaultEnvironment=LD_PRELOAD=\n", 0644));
            /* Rosetta generates executable code while running a translated
             * service. The kernel's MDWE restriction prevents that translation. */
            const char *scopes[] = {"system", "user"};
            for (unsigned i = 0; i < 2; i++) {
                char path[256];
                /* Distinct filename: systemd replaces equal-basename generic
                 * drop-ins with unit-specific ones instead of merging them. */
                snprintf(path, sizeof(path), "/etc/systemd/%s/service.d/40-nativepipe-rosetta-memory.conf", scopes[i]);
                TRY(TEXT(root, path, "[Service]\nMemoryDenyWriteExecute=no\n", 0644));
            }
            /* These services also call the missing interfaces after
             * exec. D-Bus was verified without the library once journald
             * recovered. Do not apply a wildcard to unrelated services. */
            const char *services[] = {
                "systemd-journald", "systemd-userdbd", "systemd-udevd",
                "systemd-vconsole-setup", /* Closes descriptors before loadkeys/setfont. */
            };
            for (unsigned i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
                char path[256];
                snprintf(path, sizeof(path), "/etc/systemd/system/%s.service.d/50-nativepipe-rosetta.conf", services[i]);
                TRY(TEXT(root, path, "[Service]\nEnvironment=LD_PRELOAD=" NP_ROSETTA_COMPAT_PATH "\n", 0644));
            }
        }
        if (!strcmp(install->architecture, "amd64")) {
            /* Translation is a prerequisite for PID 1 and every service.
             * systemd-binfmt globally clears handlers at start and stop;
             * it must not own the early-boot Rosetta registration. Individual
             * additional formats can still be registered through binfmt_misc. */
            TRY(np_root_link(root, "/etc/systemd/system/systemd-binfmt.service", "/dev/null"));
            /* nativepipe-init has already mounted this filesystem. An
             * automount at the same path fails and cannot own its lifetime. */
            TRY(np_root_link(root, "/etc/systemd/system/proc-sys-fs-binfmt_misc.automount", "/dev/null"));
        }
        TRY(TEXT(root, "/etc/machine-id", "", 0644));
        TRY(TEXT(root, "/etc/systemd/network/20-linportal.network",
                 "[Match]\nName=en* eth*\n\n[Network]\nDHCP=yes\nIPv6AcceptRA=yes\n", 0644));
        TRY(np_root_link(root, "/etc/resolv.conf", "/run/systemd/resolve/stub-resolv.conf"));
        TRY(systemd_enable(root, "multi-user.target", "systemd-networkd.service", NULL));
        TRY(systemd_enable(root, "multi-user.target", "systemd-resolved.service", NULL));
        TRY(systemd_enable(root, "network-online.target", "systemd-networkd-wait-online.service", NULL));
        TRY(systemd_enable(root, "getty.target", "serial-getty@hvc0.service", "serial-getty@.service"));
        char home[256];
        snprintf(home, sizeof(home), "/home/%s/.config/systemd/user/default.target.wants", install->username);
        TRY(np_root_mkdir(root, home, 0755));
        const char *services[] = {"pipewire.service", "pipewire-pulse.service", "wireplumber.service"};
        for (unsigned i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
            char link[512], source[256];
            snprintf(source, sizeof(source), "/usr/lib/systemd/user/%s", services[i]);
            int fd = np_file_open(root, source, O_RDONLY, 0);
            if (fd < 0) return -1;
            close(fd);
            snprintf(link, sizeof(link), "%s/%s", home, services[i]);
            TRY(np_root_link(root, link, source));
        }
        char chown[256], owner[80], config[256];
        TRY(np_install_program(root, "chown", chown));
        snprintf(owner, sizeof(owner), "%u:%u", (unsigned)install->uid, (unsigned)install->gid);
        snprintf(config, sizeof(config), "/home/%s/.config", install->username);
        char *args[] = {chown, "-R", owner, config, NULL};
        TRY(np_root_run(root, args, NULL, 0));
    } else {
        char uuidgen[256];
        TRY(np_install_program(root, "dbus-uuidgen", uuidgen));
        char *identity[] = {uuidgen, "--ensure=/etc/machine-id", NULL};
        TRY(np_root_run(root, identity, NULL, 0));
        TRY(TEXT(root, "/etc/network/interfaces", "auto lo\niface lo inet loopback\n\nauto eth0\niface eth0 inet dhcp\n", 0644));
        TRY(np_root_link(root, "/etc/udev/rules.d/80-net-setup-link.rules", "/dev/null"));
        TRY(TEXT(root, "/etc/inittab",
            "::sysinit:/sbin/openrc sysinit\n::sysinit:/sbin/openrc boot\n::wait:/sbin/openrc default\n"
            "hvc0::respawn:/sbin/getty -L 115200 hvc0 vt100\n::ctrlaltdel:/sbin/reboot\n"
            "::shutdown:/sbin/openrc shutdown\n", 0644));
        const char *sysinit[] = {"devfs", "dmesg", "udev", "udev-trigger"};
        const char *boot[] = {"modules", "sysctl", "hostname", "bootmisc"};
        const char *normal[] = {"dbus", "networking", "elogind"};
        for (unsigned i = 0; i < sizeof(sysinit) / sizeof(sysinit[0]); i++) TRY(openrc_enable(root, "sysinit", sysinit[i]));
        for (unsigned i = 0; i < sizeof(boot) / sizeof(boot[0]); i++) TRY(openrc_enable(root, "boot", boot[i]));
        for (unsigned i = 0; i < sizeof(normal) / sizeof(normal[0]); i++) TRY(openrc_enable(root, "default", normal[i]));
    }
    return 0;
}

int np_install_guest(struct np_install *install) {
    /* Recovery contains a static, kernel-native installer. Preserve that
     * helper for `nativepipe-install apps`; it selects packages using the
     * target /bin/sh ELF ABI, including amd64 roots on an ARM kernel. */
    int executable = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (executable < 0) return -1;
    int copied = np_root_copy(install->root, "/usr/sbin/nativepipe-install", executable, 0755);
    close(executable);
    if (copied < 0) return -1;
    const char *paths[] = {"/agent/nativepipe-guestd", "/agent/systemd/nativepipe-guestd.service", "/agent/openrc/nativepipe-guestd"};
    const char *destinations[] = {"/usr/libexec/nativepipe/nativepipe-guestd", "/etc/systemd/system/nativepipe-guestd.service", "/etc/init.d/nativepipe-guestd"};
    for (unsigned i = 0; i < 3; i++) {
        if ((i == 1 && install->distribution->init != NP_SYSTEMD) ||
            (i == 2 && install->distribution->init != NP_OPENRC)) continue;
        int fd = np_file_open(install->payload, paths[i], O_RDONLY, 0);
        if (fd < 0) return -1;
        int rc = np_root_copy(install->root, destinations[i], fd, i == 1 ? 0644 : 0755);
        close(fd);
        if (rc < 0) return -1;
    }
    TRY(TEXT(install->root, "/var/lib/nativepipe/session-user", install->username, 0644));
    if (install->rosetta && strcmp(install->architecture, "amd64")) {
        if (install->distribution->init == NP_SYSTEMD) {
            TRY(TEXT(install->root, "/etc/systemd/system/linportal-rosetta.service",
                     "[Unit]\nDescription=Mount Apple Rosetta for Linux\nBefore=nativepipe-guestd.service\n\n"
                     "[Service]\nType=oneshot\nExecStart=/usr/libexec/nativepipe/nativepipe-guestd --prepare-rosetta\nRemainAfterExit=yes\n", 0644));
            TRY(systemd_enable(install->root, "multi-user.target", "linportal-rosetta.service", NULL));
        } else {
            TRY(TEXT(install->root, "/etc/init.d/linportal-rosetta",
                     "#!/sbin/openrc-run\ncommand=/usr/libexec/nativepipe/nativepipe-guestd\n"
                     "command_args=--prepare-rosetta\ndepend() { need localmount; before nativepipe-guestd; }\n", 0755));
            TRY(openrc_enable(install->root, "default", "linportal-rosetta"));
        }
    }
    return install->distribution->init == NP_SYSTEMD
        ? systemd_enable(install->root, "multi-user.target", "nativepipe-guestd.service", NULL)
        : openrc_enable(install->root, "default", "nativepipe-guestd");
}
