#define _GNU_SOURCE
#include "install.h"
#include "root.h"
#include "np_file_rpc.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TRY(call) do { if ((call) != 0) return -1; } while (0)
#define TEXT(root, path, data, mode) np_root_write(root, path, data, strlen(data), mode)

int np_install_amd64_entry(struct np_install *native, struct np_install *translated) {
    /* schroot owns runtime credentials, terminal handling and mount teardown.
     * The C installer only creates its standard configuration. */
    TRY(np_install_package_set(native, 0, "schroot"));
    const char *files[] = {"nativepipe-align-blob-x86_64-gnu.so",
        "nativepipe-vulkan-layer-x86_64-gnu.so", "VkLayer_NATIVEPIPE_blob_alignment.json", "nativepipe.sh"};
    const char *destinations[] = {
        "/usr/libexec/nativepipe/nativepipe-align-host-blob.so",
        "/usr/libexec/nativepipe/nativepipe-vulkan-blob-alignment.so",
        "/etc/vulkan/implicit_layer.d/VkLayer_NATIVEPIPE_blob_alignment.json",
        "/etc/profile.d/nativepipe.sh"};
    for (unsigned i = 0; i < 4; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/amd64-graphics/%s", files[i]);
        int fd = np_file_open(native->payload, path, O_RDONLY, 0);
        if (fd < 0) return -1;
        int rc = np_root_copy(translated->root, destinations[i], fd, i < 2 ? 0755 : 0644);
        close(fd);
        if (rc < 0) return -1;
    }
    TRY(TEXT(translated->root, "/usr/libexec/nativepipe/amd64-session",
        "#!/bin/sh\nset -eu\n. /etc/profile.d/nativepipe.sh\n"
        "if [ \"$#\" = 0 ]; then\n"
        "  shell=$(getent passwd \"$(id -u)\" | cut -d: -f7)\n"
        "  case \"$shell\" in /*) exec \"$shell\" -l;; *) exit 1;; esac\n"
        "fi\nexec \"$@\"\n", 0755));
    char configuration[1024];
    snprintf(configuration, sizeof(configuration),
        "[nativepipe-amd64]\ndescription=%s amd64 through Rosetta\n"
        "type=directory\ndirectory=/var/lib/nativepipe/amd64\nusers=%s\n"
        "profile=nativepipe-amd64\npreserve-environment=true\n",
        translated->distribution->id, native->username);
    TRY(TEXT(native->root, "/etc/schroot/chroot.d/nativepipe-amd64.conf", configuration, 0644));
    TRY(TEXT(native->root, "/etc/schroot/nativepipe-amd64/fstab",
        "/proc /proc none bind 0 0\n/sys /sys none bind 0 0\n/dev /dev none bind 0 0\n"
        "/dev/pts /dev/pts none bind 0 0\n/dev/shm /dev/shm none bind 0 0\n"
        "/home /home none rbind,rslave 0 0\n/tmp /tmp none bind 0 0\n"
        "/run/user /run/user none rbind,rslave 0 0\n/run/rosetta /run/rosetta none bind 0 0\n"
        "/mnt/lighthouse /mnt/lighthouse none rbind,rslave 0 0\n", 0644));
    TRY(TEXT(native->root, "/etc/schroot/nativepipe-amd64/copyfiles",
        "/etc/resolv.conf\n/etc/hosts\n", 0644));
    /* Service account IDs belong to each distro. Only the interactive account
     * was matched by np_install_account; never copy the native NSS databases. */
    TRY(TEXT(native->root, "/etc/schroot/nativepipe-amd64/nssdatabases", "", 0644));
    TRY(np_root_mkdir(native->root, "/run/user", 0755));
    TRY(np_root_mkdir(native->root, "/mnt/lighthouse", 0755));
    TRY(TEXT(translated->root, "/etc/motd",
        "This amd64 environment shares the ARM64 VM kernel.\n", 0644));
    return TEXT(native->root, "/usr/local/bin/nativepipe-amd64",
        "#!/bin/sh\nset -eu\n"
        "if [ ! -r /proc/sys/fs/binfmt_misc/rosetta ] || [ ! -x /run/rosetta/rosetta ]; then\n"
        "  echo 'Enable Rosetta in FluxWindow and restart this VM.' >&2\n  exit 1\nfi\n"
        "exec /usr/bin/schroot --quiet --chroot=nativepipe-amd64 --preserve-environment "
        "-- /usr/libexec/nativepipe/amd64-session \"$@\"\n", 0755);
}
