#define _GNU_SOURCE
#include <signal.h>
#include <sys/reboot.h>
#include <unistd.h>

/* Exercise shell completion without ever signaling processes, unmounting
 * filesystems, or powering off the machine running this unit test. */
static int test_kill(pid_t pid, int signal);
static void test_sync(void);
static int test_reboot(int command);
static int test_execve(const char *path, char *const args[], char *const env[]);
#define kill test_kill
#define sync test_sync
#define reboot test_reboot
#define execve test_execve
#define main nativepipe_init_main
#include "nativepipe-init.c"
#undef main
#undef kill
#undef sync
#undef reboot
#undef execve

static int shell_exit_status;
static int shutdown_step;
static int installer_exit_status = 23;
static bool installer_reports_error;

static int test_kill(pid_t pid, int signal) {
    if (pid != -1 || signal != (shutdown_step == 0 ? SIGTERM : SIGKILL))
        abort();
    shutdown_step++;
    return 0;
}

static void test_sync(void) {
    if (shutdown_step != 2) abort();
    shutdown_step++;
}

static int test_reboot(int command) {
    if (command != RB_POWER_OFF || shutdown_step != 3) abort();
    shutdown_step++;
    return 0;
}

static int test_execve(const char *path, char *const args[], char *const env[]) {
    if (!strcmp(path, "/sbin/nativepipe-install")) {
        if (!args[1] || (strcmp(args[1], "install") && strcmp(args[1], "repair")) || args[2])
            abort();
        bool root = false, disk = false, source = false;
        for (unsigned i = 0; env && env[i]; i++) {
            root |= !strcmp(env[i], "NP_TARGET_ROOT=/newroot");
            disk |= !strcmp(env[i], "NP_TARGET_DISK=/dev/vda");
            source |= !strcmp(env[i], "NP_SOURCE_PATH=/run/nativepipe/payload/source");
            if (installer_reports_error && !strncmp(env[i], "NP_INSTALL_ERROR_FD=", 20)) {
                int fd = atoi(env[i] + 20);
                struct np_install_error report = {.code = ENETUNREACH};
                strcpy(report.message, "Installation network has no DNS: Network unreachable");
                if (write(fd, &report, sizeof(report)) != sizeof(report)) abort();
                /* A later cleanup error must not replace the original cause. */
                report.code = EBUSY;
                strcpy(report.message, "Cleanup could not unmount target");
                if (write(fd, &report, sizeof(report)) != sizeof(report)) abort();
            }
        }
        _exit(root && disk && source ? installer_exit_status : 24);
    }
    if (!strcmp(path, "/bin/sh") && !strcmp(args[1], "-l"))
        _exit(shell_exit_status);
    if (!strcmp(path, "/bin/umount") && !strcmp(args[1], "-a") &&
        !strcmp(args[2], "-r") && shutdown_step == 2)
        _exit(0);
    abort();
}

static int test_recovery_shell_exit(void) {
    for (int status = 0; status <= 7; status += 7) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) return 1;
        control_connection = sockets[0];
        struct np_plan plan;
        initialize_plan(&plan);
        plan.action = NP_ACTION_SHELL;
        plan.request_id = 42;
        shell_exit_status = status;
        shutdown_step = 0;
        bool response_sent = false;
        if (execute_plan(&plan, sockets[0], &response_sent) != 0 ||
            !response_sent || shutdown_step != 4 || control_connection != -1)
            return 1;
        uint8_t *response = NULL;
        size_t length = 0;
        if (receive_payload(sockets[1], &response, &length) < 0 || length != 12 ||
            memcmp(response, "NPOK", 4) || read_le64(response + 4) != 42)
            return 1;
        free(response);
        close(sockets[1]);
    }
    return 0;
}

static int self_test(const char *program_path) {
    struct np_plan native;
    initialize_plan(&native);
    strcpy(native.disk, "/dev/vda");
    strcpy(native.source, "/run/nativepipe/payload/source");
    if (run_installation(&native, "install") != 23 ||
        run_installation(&native, "repair") != 23) return 1;
    if (!strstr(plan_error, "status 23") || strstr(plan_error, "I/O error")) return 1;
    installer_reports_error = true;
    if (run_installation(&native, "install") != 23 || errno != ENETUNREACH ||
        strcmp(plan_error, "Installation network has no DNS: Network unreachable")) return 1;
    int errors[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, errors) < 0 ||
        send_error(errors[0], 42, ENETUNREACH, plan_error) < 0) return 1;
    uint8_t *error_payload = NULL;
    size_t error_length = 0;
    if (receive_payload(errors[1], &error_payload, &error_length) < 0 ||
        error_length < 18 || memcmp(error_payload, "NPER", 4) ||
        read_le32(error_payload + 12) != ENETUNREACH ||
        error_length != 18 + strlen(plan_error) ||
        memcmp(error_payload + 18, plan_error, strlen(plan_error))) return 1;
    free(error_payload); close(errors[0]); close(errors[1]);
    installer_reports_error = false;
    installer_exit_status = 0;
    if (run_installation(&native, "install") != 0 || plan_error[0]) return 1;
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
        30, 0, '/', 'r', 'u', 'n', '/', 'n', 'a', 't', 'i', 'v', 'e', 'p', 'i', 'p', 'e', '/', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '/', 's', 'o', 'u', 'r', 'c', 'e',
    };
    struct np_plan plan;
    if (decode_plan(payload, sizeof(payload), &plan) != 0 ||
        plan.action != NP_ACTION_INSTALL || !plan.automatic ||
        strcmp(plan.disk_identifier, "nativepipe-root") != 0 ||
        !plan.root_read_only)
        return 1;

    /* An extra script selector is not part of the current installation plan. */
    struct buffer invalid_payload = {0};
    if (append(&invalid_payload, payload, sizeof(payload)) < 0 ||
        append_string(&invalid_payload, "/run/nativepipe/payload/adapter.sh") < 0 ||
        decode_plan(invalid_payload.bytes, invalid_payload.length, &plan) == 0) return 1;
    free(invalid_payload.bytes);

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
    int result = preflight_executable_at(root, self_path, 0, NP_ELF_MACHINE);
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
    result = preflight_executable_at(root, broken_path, 0, NP_ELF_MACHINE);
    int saved = errno;
    close(root);
    unlink(broken_path);
    return result < 0 && saved == ENOEXEC ? 0 : 1;
}

static int test_preflight_architecture(void) {
    char path[] = "/tmp/nativepipe-init-abi.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    struct { Elf64_Ehdr header; Elf64_Phdr load; } image = {0};
    memcpy(image.header.e_ident, ELFMAG, SELFMAG);
    image.header.e_ident[EI_CLASS] = ELFCLASS64;
    image.header.e_ident[EI_DATA] = ELFDATA2LSB;
    image.header.e_ident[EI_VERSION] = EV_CURRENT;
    image.header.e_type = ET_EXEC;
    image.header.e_machine = EM_X86_64;
    image.header.e_version = EV_CURRENT;
    image.header.e_ehsize = sizeof(Elf64_Ehdr);
    image.header.e_phentsize = sizeof(Elf64_Phdr);
    image.header.e_phnum = 1;
    image.header.e_phoff = sizeof(Elf64_Ehdr);
    image.load.p_type = PT_LOAD;
    image.load.p_filesz = image.load.p_memsz = sizeof(image);
    int result = 1;
    if (write_full(fd, &image, sizeof(image)) < 0 || fchmod(fd, 0755) < 0) goto done;
    int root = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) goto done;
    if (preflight_executable_at(root, path, 0, EM_X86_64) == 0 &&
        preflight_executable_at(root, path, 0, EM_AARCH64) < 0 && errno == ENOEXEC) result = 0;
    /* A truncated segment must fail even when the architecture is accepted. */
    if (ftruncate(fd, sizeof(image) - 1) < 0 ||
        preflight_executable_at(root, path, 0, EM_X86_64) == 0) result = 1;
    close(root);
done:
    close(fd);
    unlink(path);
    return result;
}

#if defined(__aarch64__)
static int test_systemd_init_scope(void) {
    char path[] = "/tmp/nativepipe-init-systemd.XXXXXX";
    if (!mkdtemp(path)) return 1;
    int root = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    int result = 1;
    if (root < 0) return 1;
    if (mkdirat(root, "usr", 0755) < 0 || mkdirat(root, "usr/lib", 0755) < 0 ||
        mkdirat(root, "usr/lib/systemd", 0755) < 0 || mkdirat(root, "sbin", 0755) < 0) goto done;
    int file = openat(root, "usr/lib/systemd/systemd", O_WRONLY | O_CREAT, 0755);
    if (file < 0) goto done;
    close(file);
    if (symlinkat("/usr/lib/systemd/systemd", root, "sbin/init") < 0 ||
        !is_systemd_init(root, "/sbin/init") ||
        !is_systemd_init(root, "/usr/lib/systemd/systemd") ||
        is_systemd_init(root, "/missing-init")) goto done;
    if (unlinkat(root, "sbin/init", 0) < 0) goto done;
    file = openat(root, "sbin/init", O_WRONLY | O_CREAT, 0755);
    if (file < 0) goto done;
    close(file);
    /* Another init, even an executable with the same contents, must not
     * inherit the systemd-specific compatibility environment. */
    if (!is_systemd_init(root, "/sbin/init")) result = 0;
done:
    unlinkat(root, "sbin/init", 0);
    unlinkat(root, "usr/lib/systemd/systemd", 0);
    unlinkat(root, "sbin", AT_REMOVEDIR);
    unlinkat(root, "usr/lib/systemd", AT_REMOVEDIR);
    unlinkat(root, "usr/lib", AT_REMOVEDIR);
    unlinkat(root, "usr", AT_REMOVEDIR);
    close(root);
    rmdir(path);
    return result;
}
#endif

int main(int argc, char **argv) {
    (void)argc;
#if defined(__aarch64__)
    if (test_systemd_init_scope()) return 1;
#endif
    return self_test(argv[0]) || test_recovery_shell_exit() || test_preflight_architecture();
}
