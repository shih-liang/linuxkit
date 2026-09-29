#ifndef NP_INSTALL_DISTRO_H
#define NP_INSTALL_DISTRO_H

enum np_package_manager { NP_APK, NP_APT, NP_DNF, NP_PACMAN };
enum np_init_system { NP_OPENRC, NP_SYSTEMD };
enum np_rootfs_format { NP_ROOTFS_TAR, NP_ROOTFS_OCI, NP_ROOTFS_ARCH_BOOTSTRAP };

struct np_distribution {
    const char *id;
    const char *os_id;
    enum np_package_manager packages;
    enum np_init_system init;
    const char *shell;
    const char *base_packages;
    const char *desktop_packages;
    const char *developer_packages;
};

const struct np_distribution *np_distribution_find(const char *id);
int np_distribution_unused_hardware(const struct np_distribution *distro, const char *package);
/* Tokens are compiled package names, not shell fragments or network policy. */
int np_package_arguments(const struct np_distribution *distro, int refresh,
                         const char *packages, char **argv, unsigned capacity,
                         char *storage, unsigned storage_size);

#endif
