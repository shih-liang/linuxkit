#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NP_PAYLOAD_ROOT "/run/nativepipe/payload"
#define NP_NEW_ROOT "/newroot"

enum np_mode {
    NP_MODE_NORMAL,
    NP_MODE_INSTALL,
    NP_MODE_REPAIR,
    NP_MODE_SHELL,
};

struct np_options {
    enum np_mode mode;
    char root[256];
    char disk[256];
    char payload_tag[64];
    char adapter[256];
    char source[256];
    bool automatic;
};

static void logmsg(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    dprintf(STDERR_FILENO, "[nativepipe-init] ");
    vdprintf(STDERR_FILENO, format, arguments);
    dprintf(STDERR_FILENO, "\n");
    va_end(arguments);
}

static int copy_value(char *destination, size_t capacity, const char *value) {
    size_t length = strlen(value);
    if (length == 0 || length >= capacity)
        return -1;
    memcpy(destination, value, length + 1);
    return 0;
}

static bool has_parent_component(const char *path) {
    return strstr(path, "/../") || strcmp(path, "..") == 0 ||
           strncmp(path, "../", 3) == 0 ||
           (strlen(path) >= 3 && strcmp(path + strlen(path) - 3, "/..") == 0);
}

static bool valid_device(const char *path) {
    return strncmp(path, "/dev/", 5) == 0 && !has_parent_component(path);
}

static bool valid_root(const char *root) {
    return valid_device(root) || strncmp(root, "UUID=", 5) == 0 ||
           strncmp(root, "PARTUUID=", 9) == 0 || strncmp(root, "LABEL=", 6) == 0;
}

static bool valid_payload_path(const char *path) {
    size_t prefix = strlen(NP_PAYLOAD_ROOT);
    return strncmp(path, NP_PAYLOAD_ROOT "/", prefix + 1) == 0 &&
           !has_parent_component(path);
}

static bool valid_tag(const char *tag) {
    if (!tag[0])
        return false;
    for (const unsigned char *p = (const unsigned char *)tag; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')
            continue;
        return false;
    }
    return true;
}

static void default_options(struct np_options *options) {
    memset(options, 0, sizeof(*options));
    options->mode = NP_MODE_NORMAL;
    options->automatic = true;
    copy_value(options->payload_tag, sizeof(options->payload_tag), "nativepipe-install");
    copy_value(options->adapter, sizeof(options->adapter),
               NP_PAYLOAD_ROOT "/adapter.sh");
    copy_value(options->source, sizeof(options->source), NP_PAYLOAD_ROOT "/source");
}

static int parse_cmdline(const char *cmdline, struct np_options *options) {
    default_options(options);
    char *copy = strdup(cmdline ? cmdline : "");
    if (!copy)
        return -1;

    char *save = NULL;
    for (char *token = strtok_r(copy, " \t\r\n", &save); token;
         token = strtok_r(NULL, " \t\r\n", &save)) {
        const char *value = NULL;
        if (strncmp(token, "nativepipe.mode=", 16) == 0) {
            value = token + 16;
            if (strcmp(value, "normal") == 0)
                options->mode = NP_MODE_NORMAL;
            else if (strcmp(value, "install") == 0)
                options->mode = NP_MODE_INSTALL;
            else if (strcmp(value, "repair") == 0)
                options->mode = NP_MODE_REPAIR;
            else if (strcmp(value, "shell") == 0)
                options->mode = NP_MODE_SHELL;
            else
                goto invalid;
        } else if (strncmp(token, "nativepipe.root=", 16) == 0) {
            value = token + 16;
            if (!valid_root(value) || copy_value(options->root, sizeof(options->root), value) < 0)
                goto invalid;
        } else if (strncmp(token, "root=", 5) == 0) {
            value = token + 5;
            if (!valid_root(value) || copy_value(options->root, sizeof(options->root), value) < 0)
                goto invalid;
        } else if (strncmp(token, "nativepipe.disk=", 16) == 0) {
            value = token + 16;
            if (!valid_device(value) || copy_value(options->disk, sizeof(options->disk), value) < 0)
                goto invalid;
        } else if (strncmp(token, "nativepipe.payload_tag=", 23) == 0) {
            value = token + 23;
            if (!valid_tag(value) ||
                copy_value(options->payload_tag, sizeof(options->payload_tag), value) < 0)
                goto invalid;
        } else if (strncmp(token, "nativepipe.adapter=", 19) == 0) {
            value = token + 19;
            if (!valid_payload_path(value) ||
                copy_value(options->adapter, sizeof(options->adapter), value) < 0)
                goto invalid;
        } else if (strncmp(token, "nativepipe.source=", 18) == 0) {
            value = token + 18;
            if (!valid_payload_path(value) ||
                copy_value(options->source, sizeof(options->source), value) < 0)
                goto invalid;
        } else if (strcmp(token, "nativepipe.manual") == 0) {
            options->automatic = false;
        }
    }
    free(copy);
    return 0;

invalid:
    free(copy);
    errno = EINVAL;
    return -1;
}

static void make_directory(const char *path, mode_t mode) {
    if (mkdir(path, mode) < 0 && errno != EEXIST) {
        logmsg("mkdir %s failed: %s", path, strerror(errno));
        _exit(1);
    }
}

static int mount_once(const char *source, const char *target, const char *filesystem,
                      unsigned long flags, const char *data) {
    if (mount(source, target, filesystem, flags, data) == 0 || errno == EBUSY)
        return 0;
    logmsg("mount %s on %s failed: %s", source, target, strerror(errno));
    return -1;
}

static void setup_runtime(void) {
    umask(022);
    make_directory("/dev", 0755);
    make_directory("/proc", 0555);
    make_directory("/sys", 0555);
    make_directory("/run", 0755);
    make_directory(NP_NEW_ROOT, 0755);

    mount_once("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    int console = open("/dev/console", O_RDWR | O_NOCTTY);
    if (console >= 0) {
        dup2(console, STDIN_FILENO);
        dup2(console, STDOUT_FILENO);
        dup2(console, STDERR_FILENO);
        if (console > STDERR_FILENO)
            close(console);
    }
    mount_once("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    mount_once("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    mount_once("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
    make_directory("/run/nativepipe", 0755);
    make_directory(NP_PAYLOAD_ROOT, 0755);
}

static int wait_child(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

static int run(char *const argv[]) {
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execv(argv[0], argv);
        dprintf(STDERR_FILENO, "[nativepipe-init] exec %s failed: %s\n", argv[0],
                strerror(errno));
        _exit(127);
    }
    return wait_child(child);
}

static bool virtio_block_name(const char *name) {
    if (strncmp(name, "vd", 2) != 0 || !name[2])
        return false;
    for (const unsigned char *p = (const unsigned char *)name + 2; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
            continue;
        return false;
    }
    return true;
}

static bool sysfs_attribute_exists(const char *name, const char *attribute) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/block/%s/%s", name, attribute);
    return access(path, F_OK) == 0;
}

static bool block_device_is_read_only(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/block/%s/ro", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return true;
    char value = '1';
    ssize_t count = read(fd, &value, 1);
    close(fd);
    return count != 1 || value != '0';
}

static int find_single_install_disk(char *destination, size_t capacity) {
    DIR *directory = opendir("/sys/class/block");
    if (!directory)
        return -1;
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!virtio_block_name(entry->d_name) ||
            sysfs_attribute_exists(entry->d_name, "partition") ||
            block_device_is_read_only(entry->d_name))
            continue;
        char candidate[256];
        snprintf(candidate, sizeof(candidate), "/dev/%s", entry->d_name);
        if (++count == 1)
            copy_value(destination, capacity, candidate);
    }
    closedir(directory);
    if (count == 1)
        return 0;
    errno = count == 0 ? ENODEV : ENOTUNIQ;
    return -1;
}

static int resolve_install_disk(struct np_options *options) {
    if (options->disk[0])
        return 0;
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        if (find_single_install_disk(options->disk, sizeof(options->disk)) == 0) {
            logmsg("selected the only writable virtio disk: %s", options->disk);
            return 0;
        }
        if (errno == ENOTUNIQ)
            break;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
    logmsg("cannot select an install disk safely; pass nativepipe.disk=/dev/vdX");
    return -1;
}

static int mount_payload(const struct np_options *options) {
    return mount_once(options->payload_tag, NP_PAYLOAD_ROOT, "virtiofs",
                      MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
}

static int run_adapter(const struct np_options *options, const char *action) {
    if (access(options->adapter, R_OK) < 0) {
        logmsg("adapter is unavailable: %s", options->adapter);
        return -1;
    }

    char disk[sizeof(options->disk) + 16];
    char source[sizeof(options->source) + 18];
    snprintf(disk, sizeof(disk), "NP_TARGET_DISK=%s", options->disk);
    snprintf(source, sizeof(source), "NP_SOURCE_PATH=%s", options->source);
    char *const environment[] = {
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin",
        "HOME=/",
        "TERM=linux",
        "NP_TARGET_ROOT=" NP_NEW_ROOT,
        disk,
        source,
        options->automatic ? "NP_AUTOMATIC=1" : "NP_AUTOMATIC=0",
        NULL,
    };
    char *const arguments[] = {
        "/bin/sh", (char *)options->adapter, (char *)action, NULL,
    };

    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execve("/bin/sh", arguments, environment);
        dprintf(STDERR_FILENO, "[nativepipe-init] cannot start adapter: %s\n",
                strerror(errno));
        _exit(127);
    }
    int result = wait_child(child);
    if (result != 0)
        logmsg("adapter action %s failed with status %d", action, result);
    return result;
}

static bool root_is_mounted(void) {
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (!mounts)
        return false;
    bool found = false;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, mounts) >= 0) {
        if (strstr(line, " " NP_NEW_ROOT " ")) {
            found = true;
            break;
        }
    }
    free(line);
    fclose(mounts);
    return found;
}

static int probe_installed_root(char *destination, size_t capacity) {
    make_directory("/run/nativepipe/probe", 0755);
    DIR *directory = opendir("/sys/class/block");
    if (!directory)
        return -1;
    unsigned matches = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!virtio_block_name(entry->d_name) ||
            !sysfs_attribute_exists(entry->d_name, "partition"))
            continue;
        char device[256];
        snprintf(device, sizeof(device), "/dev/%s", entry->d_name);
        char *const mount_arguments[] = {
            "/bin/mount", "-o", "ro", device, "/run/nativepipe/probe", NULL,
        };
        if (run(mount_arguments) != 0)
            continue;
        if (access("/run/nativepipe/probe/etc/nativepipe/installation", R_OK) == 0) {
            if (++matches == 1)
                copy_value(destination, capacity, device);
        }
        char *const unmount_arguments[] = {
            "/bin/umount", "/run/nativepipe/probe", NULL,
        };
        run(unmount_arguments);
    }
    closedir(directory);
    if (matches == 1)
        return 0;
    errno = matches == 0 ? ENOENT : ENOTUNIQ;
    return -1;
}

static int mount_root(struct np_options *options) {
    make_directory(NP_NEW_ROOT, 0755);
    if (root_is_mounted())
        return 0;
    if (!options->root[0]) {
        if (probe_installed_root(options->root, sizeof(options->root)) < 0) {
            logmsg("installed root marker was not found; pass root=PARTUUID=...");
            return -1;
        }
        logmsg("found installed root at %s", options->root);
    }
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        char *const arguments[] = {
            "/bin/mount", "-o", "rw", (char *)options->root, NP_NEW_ROOT, NULL,
        };
        int result = run(arguments);
        if (result == 0)
            return 0;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
    logmsg("could not mount root %s", options->root);
    return -1;
}

static void switch_to_root(struct np_options *options) {
    if (mount_root(options) < 0)
        return;
    if (access(NP_NEW_ROOT "/sbin/init", X_OK) < 0) {
        logmsg("%s/sbin/init is missing", NP_NEW_ROOT);
        return;
    }

    make_directory(NP_NEW_ROOT "/dev", 0755);
    make_directory(NP_NEW_ROOT "/proc", 0555);
    make_directory(NP_NEW_ROOT "/sys", 0555);
    make_directory(NP_NEW_ROOT "/run", 0755);
    mount("/dev", NP_NEW_ROOT "/dev", NULL, MS_MOVE, NULL);
    mount("/proc", NP_NEW_ROOT "/proc", NULL, MS_MOVE, NULL);
    mount("/sys", NP_NEW_ROOT "/sys", NULL, MS_MOVE, NULL);
    mount("/run", NP_NEW_ROOT "/run", NULL, MS_MOVE, NULL);

    logmsg("switching to %s", options->root);
    char *const arguments[] = {
        "/bin/switch_root", "-c", "/dev/console", NP_NEW_ROOT, "/sbin/init", NULL,
    };
    execv(arguments[0], arguments);
    logmsg("switch_root failed: %s", strerror(errno));
}

static void emergency_shell(void) {
    logmsg("starting emergency shell; exit respawns it");
    for (;;) {
        char *const arguments[] = {"/bin/sh", "-l", NULL};
        run(arguments);
        sleep(1);
    }
}

static int read_cmdline(char *buffer, size_t capacity) {
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t count = read(fd, buffer, capacity - 1);
    int saved = errno;
    close(fd);
    if (count < 0) {
        errno = saved;
        return -1;
    }
    buffer[count] = '\0';
    return 0;
}

static int self_test(void) {
    struct np_options options;
    if (parse_cmdline("", &options) < 0 || options.mode != NP_MODE_NORMAL ||
        options.root[0] || options.disk[0])
        return 1;
    if (parse_cmdline("nativepipe.mode=install root=UUID=abcd nativepipe.manual", &options) < 0 ||
        options.mode != NP_MODE_INSTALL || options.automatic ||
        strcmp(options.root, "UUID=abcd") != 0)
        return 1;
    if (parse_cmdline("nativepipe.mode=repair nativepipe.disk=/dev/vdb", &options) < 0 ||
        options.mode != NP_MODE_REPAIR || strcmp(options.disk, "/dev/vdb") != 0)
        return 1;
    if (parse_cmdline("nativepipe.disk=/dev/../host", &options) == 0)
        return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();

    setup_runtime();
    if (getpid() != 1)
        logmsg("warning: expected to run as PID 1");

    char cmdline[4096];
    struct np_options options;
    if (read_cmdline(cmdline, sizeof(cmdline)) < 0 ||
        parse_cmdline(cmdline, &options) < 0) {
        logmsg("invalid kernel command line: %s", strerror(errno));
        emergency_shell();
    }

    if (options.mode == NP_MODE_SHELL) {
        mount_payload(&options);
        emergency_shell();
    }
    if (options.mode == NP_MODE_INSTALL || options.mode == NP_MODE_REPAIR) {
        const char *action = options.mode == NP_MODE_INSTALL ? "install" : "repair";
        if (resolve_install_disk(&options) < 0 || mount_payload(&options) < 0 ||
            run_adapter(&options, action) != 0)
            emergency_shell();
    }

    switch_to_root(&options);
    emergency_shell();
}
