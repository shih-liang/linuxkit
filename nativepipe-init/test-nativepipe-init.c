#define main nativepipe_init_main
#include "nativepipe-init.c"
#undef main

static int self_test(const char *program_path) {
    char device_path[] = "/tmp/nativepipe-init-dev.XXXXXX";
    if (!mkdtemp(device_path))
        return 1;
    int device_directory = open(device_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool links_ok = device_directory >= 0 && setup_fd_links(device_directory) == 0 &&
                    setup_fd_links(device_directory) == 0;
    for (size_t i = 0; i < sizeof(standard_fd_links) / sizeof(standard_fd_links[0]); i++) {
        char target[64] = {0};
        ssize_t count = readlinkat(device_directory, standard_fd_links[i].name,
                                   target, sizeof(target) - 1);
        if (count < 0 || strcmp(target, standard_fd_links[i].target) != 0)
            links_ok = false;
        unlinkat(device_directory, standard_fd_links[i].name, 0);
    }
    if (device_directory >= 0)
        close(device_directory);
    rmdir(device_path);
    if (!links_ok)
        return 1;

    uint8_t payload[] = {
        'N', 'P', 'I', 'C', 1, 0, 0, 0, 0, 0, 0, 0,
        NP_ACTION_INSTALL, 1, 0, 0,
        15, 0, 'n', 'a', 't', 'i', 'v', 'e', 'p', 'i', 'p', 'e', '-', 'r', 'o', 'o', 't',
        0, 0,
        18, 0, 'n', 'a', 't', 'i', 'v', 'e', 'p', 'i', 'p', 'e', '-', 'i', 'n', 's', 't', 'a', 'l', 'l',
        34, 0, '/', 'r', 'u', 'n', '/', 'n', 'a', 't', 'i', 'v', 'e', 'p', 'i', 'p', 'e', '/', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '/', 'a', 'd', 'a', 'p', 't', 'e', 'r', '.', 's', 'h',
        30, 0, '/', 'r', 'u', 'n', '/', 'n', 'a', 't', 'i', 'v', 'e', 'p', 'i', 'p', 'e', '/', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '/', 's', 'o', 'u', 'r', 'c', 'e',
    };
    struct np_plan plan;
    if (decode_plan(payload, sizeof(payload), &plan) != 0 ||
        plan.action != NP_ACTION_INSTALL || !plan.automatic ||
        strcmp(plan.disk_identifier, "nativepipe-root") != 0 ||
        !plan.root_read_only)
        return 1;

    char valid[] =
        "console=hvc0 root=PARTUUID=1234-02 rootfstype=ext4 "
        "rootflags=noatime ro rootwait=9 rootdelay=2 init=/lib/systemd/systemd "
        "nativepipe.memory_target_bytes=2147483648";
    bool maintenance = false;
    uint64_t target_memory = 0;
    if (parse_boot_configuration(valid, &maintenance, &plan, &target_memory) < 0 ||
        maintenance || strcmp(plan.root, "PARTUUID=1234-02") ||
        strcmp(plan.root_fstype, "ext4") || strcmp(plan.root_flags, "noatime") ||
        !plan.root_read_only || !plan.root_wait || plan.root_wait_seconds != 9 ||
        plan.root_delay_seconds != 2 || strcmp(plan.init, "/lib/systemd/systemd") ||
        target_memory != UINT64_C(2147483648))
        return 1;

    char alternatives[] =
        "root=PARTLABEL=nativepipe-root rootfstype=ext4,xfs ro rw rootwait rootwait=4";
    if (parse_boot_configuration(
            alternatives, &maintenance, &plan, &target_memory) < 0 ||
        strcmp(plan.root, "PARTLABEL=nativepipe-root") ||
        strcmp(plan.root_fstype, "ext4,xfs") || plan.root_read_only ||
        !plan.root_wait || plan.root_wait_seconds != 4)
        return 1;

    char invalid[] = "nativepipe.memory_target_bytes=not-a-number";
    if (parse_boot_configuration(invalid, &maintenance, &plan, &target_memory) >= 0)
        return 1;
    char unaligned[] = "nativepipe.memory_target_bytes=1048577";
    if (parse_boot_configuration(unaligned, &maintenance, &plan, &target_memory) >= 0)
        return 1;

    char self_path[PATH_MAX];
    if (!realpath(program_path, self_path))
        return 1;
    int root = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root < 0)
        return 1;
    int result = preflight_executable_at(root, self_path, 0);
    close(root);
    if (result < 0)
        return 1;

    char broken_path[] = "/tmp/nativepipe-init-broken.XXXXXX";
    int broken = mkstemp(broken_path);
    if (broken < 0 || fchmod(broken, 0755) < 0 ||
        write_full(broken, "not an ELF", sizeof("not an ELF") - 1) < 0) {
        if (broken >= 0)
            close(broken);
        unlink(broken_path);
        return 1;
    }
    close(broken);
    root = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) {
        unlink(broken_path);
        return 1;
    }
    result = preflight_executable_at(root, broken_path, 0);
    int saved = errno;
    close(root);
    unlink(broken_path);
    return result < 0 && saved == ENOEXEC ? 0 : 1;
}

int main(int argc, char **argv) {
    (void)argc;
    return self_test(argv[0]);
}
