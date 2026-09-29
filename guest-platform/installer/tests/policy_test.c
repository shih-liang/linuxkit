#undef NDEBUG
#include "distro.h"
#include "rosetta.h"
#include "os_release.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char release[128];
    assert(np_os_release_value("ID='alpine'\nNAME=\"Alpine Linux\"\n", "ID", release, sizeof(release)) == 1);
    assert(!strcmp(release, "alpine"));
    assert(np_os_release_value("NAME=\"quoted \\\"name\\\"\"\n", "NAME", release, sizeof(release)) == 1);
    assert(!strcmp(release, "quoted \"name\""));
    assert(np_os_release_value("ID=$(touch /tmp/no-evaluation)\n", "ID", release, sizeof(release)) == 1);
    assert(!strcmp(release, "$(touch /tmp/no-evaluation)"));
    assert(np_os_release_value("ID=\"broken\n", "ID", release, sizeof(release)) < 0);
    assert(np_os_release_value("ID=alpine\n", "VERSION", release, sizeof(release)) == 0);
    const char *handler = "enabled\ninterpreter /run/rosetta/rosetta\nflags: POCF\noffset 0\n"
        "magic 7f454c4602010100000000000000000002003e00\n"
        "mask fffffffffffefe00fffffffffffffffffeffffff\n";
    assert(np_rosetta_handler_matches(handler));
    char changed[512];
    strcpy(changed, handler);
    *strstr(changed, "offset 0") = 'x';
    assert(!np_rosetta_handler_matches(changed));
    assert(!np_rosetta_handler_matches(NULL));
    assert(strstr(np_rosetta_registration(), "\\x7fELF") != NULL);
    assert(strstr(np_rosetta_registration(), ":POCF\n") != NULL);
    assert(np_distribution_find("not-a-distro") == NULL);
    const struct np_distribution *arch = np_distribution_find("archlinux-arm");
    assert(np_distribution_unused_hardware(arch, "linux-aarch64"));
    assert(np_distribution_unused_hardware(arch, "linux-firmware"));
    assert(np_distribution_unused_hardware(arch, "linux-firmware-nvidia"));
    assert(!np_distribution_unused_hardware(arch, "linux-api-headers"));
    assert(!np_distribution_unused_hardware(arch, "mesa"));
    assert(!np_distribution_unused_hardware(arch, "vulkan-virtio"));
    assert(!np_distribution_unused_hardware(arch, "kmod"));
    assert(!np_distribution_unused_hardware(arch, "linux-firmwarex"));
    char *argv[128], storage[8192];
    const struct np_distribution *ubuntu = np_distribution_find("ubuntu");
    const struct np_distribution *debian = np_distribution_find("debian");
    assert(ubuntu && debian && ubuntu->packages == debian->packages);
    assert(np_package_arguments(ubuntu, 0, "curl git", argv, 128, storage, sizeof(storage)) == 6);
    assert(!strcmp(argv[0], "/usr/bin/apt-get"));
    assert(!strcmp(argv[4], "curl") && !strcmp(argv[5], "git") && argv[6] == NULL);
    assert(np_package_arguments(ubuntu, 0, "-bad", argv, 128, storage, sizeof(storage)) < 0);
    const struct np_distribution *arch64 = np_distribution_find("archlinux");
    assert(arch64 && !strcmp(arch64->os_id, "arch"));
    assert(!strcmp(arch64->architecture, "amd64") && arch64->rootfs_format == NP_ROOTFS_ARCH_BOOTSTRAP);
    assert(!strcmp(arch64->pacman_keyring, "archlinux") && !strcmp(arch->pacman_keyring, "archlinuxarm"));
    assert(strstr(arch64->base_packages, "systemd-sysvcompat"));
    const char *ids[] = {"alpine", "debian", "ubuntu", "fedora", "archlinux-arm", "archlinux"};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const struct np_distribution *d = np_distribution_find(ids[i]);
        assert(d);
        assert(np_package_arguments(d, 0, d->base_packages, argv, 128, storage, sizeof(storage)) > 0);
        assert(np_package_arguments(d, 0, d->desktop_packages, argv, 128, storage, sizeof(storage)) > 0);
    }
    puts("installer policy and Rosetta handler validation passed");
    return 0;
}
