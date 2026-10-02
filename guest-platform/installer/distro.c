#include "distro.h"

#include <errno.h>
#include <string.h>

#define DEB_BASE "systemd-sysv systemd-resolved udev dbus-user-session iproute2 util-linux e2fsprogs kmod passwd sudo ca-certificates libpam-systemd libpam-modules libpam-runtime"
#define DEB_DESKTOP "pipewire-audio rtkit xdg-user-dirs gsettings-desktop-schemas fonts-dejavu-core adwaita-icon-theme libglib2.0-0t64 libgdk-pixbuf-2.0-0 librsvg2-2 libgl1 libgles2 libegl1 libgl1-mesa-dri libegl-mesa0 libglx-mesa0 mesa-vulkan-drivers libwayland-server0 libxkbcommon0 libxcb1 libxcb-cursor0 xwayland"
#define ARCH_BASE "systemd iproute2 util-linux e2fsprogs kmod shadow sudo ca-certificates dbus"
#define ARCH_DESKTOP "xdg-user-dirs gsettings-desktop-schemas ttf-dejavu adwaita-icon-theme glib2 gdk-pixbuf2 librsvg pipewire pipewire-audio pipewire-alsa pipewire-pulse wireplumber rtkit realtime-privileges mesa vulkan-virtio wayland libxkbcommon libxcb xcb-util-cursor vulkan-icd-loader xorg-xwayland xwayland-satellite"

static const struct np_distribution distributions[] = {
    {"alpine", "alpine", NP_APK, NP_OPENRC, "/bin/ash",
     "alpine-base openrc shadow sudo ca-certificates iproute2 util-linux e2fsprogs dbus dbus-openrc eudev eudev-openrc kmod",
     "pipewire pipewire-pulse wireplumber elogind elogind-openrc xdg-user-dirs gsettings-desktop-schemas font-dejavu adwaita-icon-theme glib gdk-pixbuf librsvg mesa-dri-gallium mesa-egl mesa-gl mesa-gles mesa-vulkan-virtio vulkan-loader wayland libxkbcommon libxcb xcb-util-cursor xwayland xwayland-satellite",
     NULL, NP_ROOTFS_TAR, NULL},
    {"debian", "debian", NP_APT, NP_SYSTEMD, "/bin/bash", DEB_BASE, DEB_DESKTOP,
     NULL, NP_ROOTFS_TAR, NULL},
    {"ubuntu", "ubuntu", NP_APT, NP_SYSTEMD, "/bin/bash", DEB_BASE, DEB_DESKTOP,
     NULL, NP_ROOTFS_TAR, NULL},
    {"fedora", "fedora", NP_DNF, NP_SYSTEMD, "/bin/bash",
     "systemd systemd-pam systemd-udev systemd-networkd systemd-resolved util-linux e2fsprogs shadow-utils iproute dbus-daemon sudo ca-certificates kmod",
     "xdg-user-dirs gsettings-desktop-schemas dejavu-sans-fonts adwaita-icon-theme glib2 gdk-pixbuf2 librsvg2 pipewire pipewire-alsa pipewire-pulseaudio wireplumber rtkit mesa-dri-drivers mesa-libEGL mesa-libGL mesa-vulkan-drivers libglvnd-gles libwayland-client libwayland-server libxkbcommon libxcb xcb-util-cursor vulkan-loader xorg-x11-server-Xwayland xwayland-satellite",
     NULL, NP_ROOTFS_OCI, NULL},
    {"archlinux-arm", "archarm", NP_PACMAN, NP_SYSTEMD, "/bin/bash",
     ARCH_BASE, ARCH_DESKTOP, "arm64", NP_ROOTFS_TAR, "archlinuxarm"},
    {"archlinux", "arch", NP_PACMAN, NP_SYSTEMD, "/bin/bash",
     ARCH_BASE " systemd-sysvcompat", ARCH_DESKTOP,
     "amd64", NP_ROOTFS_ARCH_BOOTSTRAP, "archlinux"},
};

const struct np_distribution *np_distribution_find(const char *id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < sizeof(distributions) / sizeof(distributions[0]); i++)
        if (!strcmp(id, distributions[i].id)) return &distributions[i];
    return NULL;
}

int np_distribution_unused_hardware(const struct np_distribution *d, const char *name) {
    if (!d || !name || d->packages != NP_PACMAN) return 0;
    /* The ARM publisher's general-purpose image includes a physical-machine
     * kernel and firmware. LinPortal supplies its own kernel and VirtIO
     * devices. Keep headers, kmod and Mesa: userspace still needs them. */
    return !strcmp(name, "linux-aarch64") || !strcmp(name, "linux-firmware") ||
        !strncmp(name, "linux-firmware-", 15) || !strcmp(name, "firmware-raspberrypi");
}

int np_package_arguments(const struct np_distribution *d, int refresh,
                         const char *packages, char **argv, unsigned capacity,
                         char *storage, unsigned storage_size) {
    if (!d || !argv || capacity < 8 || !storage || !packages ||
        strlen(packages) >= storage_size) { errno = EINVAL; return -1; }
    unsigned n = 0;
    switch (d->packages) {
    case NP_APK:
        argv[n++] = "/sbin/apk";
        argv[n++] = refresh ? "update" : "add";
        break;
    case NP_APT:
        argv[n++] = "/usr/bin/apt-get";
        if (refresh) argv[n++] = "update";
        else {
            argv[n++] = "install"; argv[n++] = "-y";
            argv[n++] = "--no-install-recommends";
        }
        break;
    case NP_DNF:
        argv[n++] = "/usr/bin/dnf"; argv[n++] = "-y";
        argv[n++] = "--setopt=install_weak_deps=False";
        argv[n++] = refresh ? "makecache" : "install";
        break;
    case NP_PACMAN:
        argv[n++] = "/usr/bin/pacman"; argv[n++] = refresh ? "-Syu" : "-S";
        argv[n++] = "--noconfirm"; argv[n++] = "--needed";
        break;
    }
    memcpy(storage, packages, strlen(packages) + 1);
    char *p = storage;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (n + 1 >= capacity || *p == '-') { errno = E2BIG; return -1; }
        argv[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    argv[n] = NULL;
    return (int)n;
}
