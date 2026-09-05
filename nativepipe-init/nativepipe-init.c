#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NP_INIT_PORT 1024u
#define NP_FRAME_HEADER_SIZE 12u
#define NP_MAX_PAYLOAD (8u * 1024u * 1024u)
#define NP_MEMORY_ALIGNMENT_BYTES (UINT64_C(1024) * UINT64_C(1024))
#define NP_PAYLOAD_ROOT "/run/nativepipe/payload"
#define NP_TARGET_MEMORY_PATH "/run/nativepipe/target-memory-bytes"
#define NP_NEW_ROOT "/newroot"
#define NP_DEFAULT_ROOT_WAIT_MILLISECONDS UINT64_C(5000)

#if defined(__aarch64__)
#define NP_ELF_MACHINE EM_AARCH64
#elif defined(__x86_64__)
#define NP_ELF_MACHINE EM_X86_64
#else
#error "nativepipe-init executable preflight needs the target ELF machine"
#endif

enum np_action {
    NP_ACTION_BOOT = 0,
    NP_ACTION_INSTALL = 1,
    NP_ACTION_REPAIR = 2,
    NP_ACTION_SHELL = 3,
};

struct np_plan {
    uint64_t request_id;
    enum np_action action;
    bool automatic;
    bool root_read_only;
    bool root_wait;
    uint32_t root_wait_seconds;
    uint32_t root_delay_seconds;
    char disk_identifier[21];
    char disk[256];
    char root[256];
    char root_fstype[64];
    char root_flags[512];
    char init[256];
    char payload_tag[64];
    char adapter[256];
    char source[256];
};

struct reader {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
};

struct buffer {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
};

static int control_connection = -1;
extern char **environ;

static void initialize_plan(struct np_plan *plan) {
    memset(plan, 0, sizeof(*plan));
    /* Linux's standard root mount default is read-only unless `rw` is set. */
    plan->root_read_only = true;
    memcpy(plan->init, "/sbin/init", sizeof("/sbin/init"));
}

static void close_control_transport(void) {
    if (control_connection >= 0) {
        shutdown(control_connection, SHUT_RDWR);
        close(control_connection);
        control_connection = -1;
    }
}

static void logmsg(const char *message) {
    dprintf(STDERR_FILENO, "[nativepipe-init] %s\n", message);
}

static void log_errno(const char *operation) {
    dprintf(STDERR_FILENO, "[nativepipe-init] %s: %s\n", operation, strerror(errno));
}

static int make_directory(const char *path, mode_t mode) {
    return mkdir(path, mode) == 0 || errno == EEXIST ? 0 : -1;
}

static int mount_once(const char *source, const char *target, const char *filesystem,
                      unsigned long flags, const char *data) {
    if (mount(source, target, filesystem, flags, data) == 0 || errno == EBUSY)
        return 0;
    log_errno(target);
    return -1;
}

static const struct {
    const char *name;
    const char *target;
} standard_fd_links[] = {
    {"fd", "/proc/self/fd"},
    {"stdin", "/proc/self/fd/0"},
    {"stdout", "/proc/self/fd/1"},
    {"stderr", "/proc/self/fd/2"},
};

static int setup_fd_links(int directory) {
    for (size_t i = 0; i < sizeof(standard_fd_links) / sizeof(standard_fd_links[0]); i++) {
        if (symlinkat(standard_fd_links[i].target, directory,
                      standard_fd_links[i].name) < 0 && errno != EEXIST)
            return -1;
    }
    return 0;
}

static int setup_runtime(void) {
    umask(022);
    signal(SIGPIPE, SIG_IGN);
    if (make_directory("/dev", 0755) < 0 ||
        make_directory("/proc", 0555) < 0 ||
        make_directory("/sys", 0555) < 0 ||
        make_directory("/run", 0755) < 0 ||
        make_directory("/tmp", 01777) < 0 ||
        make_directory(NP_NEW_ROOT, 0755) < 0 ||
        mount_once("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755") < 0)
        return -1;
    int console = open("/dev/console", O_RDWR | O_NOCTTY);
    if (console >= 0) {
        dup2(console, STDIN_FILENO);
        dup2(console, STDOUT_FILENO);
        dup2(console, STDERR_FILENO);
        if (console > STDERR_FILENO)
            close(console);
    }
    if (mount_once("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0)
        return -1;
    // devtmpfs supplies device nodes, not these userspace links. Shell process
    // substitution (including pacman-key) needs them inside installation chroots.
    int device_directory = open("/dev", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (device_directory < 0 || setup_fd_links(device_directory) < 0) {
        log_errno("create /dev standard file descriptor links");
        if (device_directory >= 0)
            close(device_directory);
        return -1;
    }
    close(device_directory);
    if (make_directory("/dev/pts", 0755) < 0 ||
        mount_once("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
                   "mode=0620,ptmxmode=0666") < 0 ||
        mount_once("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0 ||
        mount_once("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") < 0 ||
        mount_once("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") < 0 ||
        make_directory("/run/nativepipe", 0755) < 0 ||
        make_directory(NP_PAYLOAD_ROOT, 0755) < 0)
        return -1;
    return 0;
}

static int read_full(int fd, void *buffer, size_t length) {
    uint8_t *bytes = buffer;
    while (length) {
        ssize_t count = read(fd, bytes, length);
        if (count > 0) {
            bytes += count;
            length -= (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int write_full(int fd, const void *buffer, size_t length) {
    const uint8_t *bytes = buffer;
    while (length) {
        ssize_t count = write(fd, bytes, length);
        if (count > 0) {
            bytes += count;
            length -= (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int publish_target_memory(uint64_t bytes) {
    char value[32];
    int length = snprintf(value, sizeof(value), "%llu\n", (unsigned long long)bytes);
    if (length <= 0 || (size_t)length >= sizeof(value)) {
        errno = EOVERFLOW;
        return -1;
    }
    int fd = open(NP_TARGET_MEMORY_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    int result = write_full(fd, value, (size_t)length);
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; index++)
        value |= (uint64_t)bytes[index] << (index * 8);
    return value;
}

static int take_bytes(struct reader *reader, void *destination, size_t length) {
    if (reader->offset > reader->length || length > reader->length - reader->offset)
        return -1;
    memcpy(destination, reader->bytes + reader->offset, length);
    reader->offset += length;
    return 0;
}

static int take_string(struct reader *reader, char *destination, size_t capacity) {
    uint8_t length_bytes[2];
    if (take_bytes(reader, length_bytes, sizeof(length_bytes)) < 0)
        return -1;
    size_t length = read_le16(length_bytes);
    if (length >= capacity || length > reader->length - reader->offset ||
        memchr(reader->bytes + reader->offset, '\0', length))
        return -1;
    memcpy(destination, reader->bytes + reader->offset, length);
    destination[length] = '\0';
    reader->offset += length;
    return 0;
}

static bool has_parent_component(const char *path) {
    return strstr(path, "/../") || strcmp(path, "..") == 0 ||
           strncmp(path, "../", 3) == 0 ||
           (strlen(path) >= 3 && strcmp(path + strlen(path) - 3, "/..") == 0);
}

static bool valid_token(const char *value, size_t maximum) {
    size_t length = strlen(value);
    if (!length || length > maximum)
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x21 || *p > 0x7e)
            return false;
    }
    return true;
}

static bool valid_root(const char *root) {
    if (strncmp(root, "/dev/", 5) == 0)
        return !has_parent_component(root) && valid_token(root, 255);
    const char *value = NULL;
    if (strncmp(root, "UUID=", 5) == 0)
        value = root + 5;
    else if (strncmp(root, "PARTUUID=", 9) == 0)
        value = root + 9;
    else if (strncmp(root, "PARTLABEL=", 10) == 0)
        value = root + 10;
    else if (strncmp(root, "LABEL=", 6) == 0)
        value = root + 6;
    return value && valid_token(value, 246);
}

static bool valid_fstype(const char *value) {
    size_t length = strlen(value);
    if (!length || length >= sizeof(((struct np_plan *)0)->root_fstype))
        return false;
    bool component = false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == ',') {
            if (!component)
                return false;
            component = false;
        } else if (isalnum(*p) || *p == '_' || *p == '-' || *p == '.') {
            component = true;
        } else {
            return false;
        }
    }
    return component;
}

static bool valid_root_flags(const char *value) {
    if (strlen(value) >= sizeof(((struct np_plan *)0)->root_flags))
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x21 || *p > 0x7e)
            return false;
    }
    return true;
}

static bool valid_init(const char *path) {
    return path[0] == '/' && !has_parent_component(path) && valid_token(path, 255);
}

static bool valid_payload_path(const char *path) {
    size_t prefix = strlen(NP_PAYLOAD_ROOT);
    return strncmp(path, NP_PAYLOAD_ROOT "/", prefix + 1) == 0 &&
           !has_parent_component(path);
}

static int decode_plan(const uint8_t *payload, size_t length, struct np_plan *plan) {
    initialize_plan(plan);
    if (length < 16 || memcmp(payload, "NPIC", 4) != 0)
        return -1;
    struct reader reader = {payload, length, 4};
    uint8_t fixed[12];
    if (take_bytes(&reader, fixed, sizeof(fixed)) < 0)
        return -1;
    plan->request_id = read_le64(fixed);
    plan->action = (enum np_action)fixed[8];
    plan->automatic = (fixed[9] & 1u) != 0;
    if (plan->action > NP_ACTION_SHELL || fixed[10] || fixed[11] ||
        take_string(&reader, plan->disk_identifier, sizeof(plan->disk_identifier)) < 0 ||
        take_string(&reader, plan->root, sizeof(plan->root)) < 0 ||
        take_string(&reader, plan->payload_tag, sizeof(plan->payload_tag)) < 0 ||
        take_string(&reader, plan->adapter, sizeof(plan->adapter)) < 0 ||
        take_string(&reader, plan->source, sizeof(plan->source)) < 0 ||
        reader.offset != reader.length)
        return -1;

    if (plan->action == NP_ACTION_SHELL)
        return 0;
    if (plan->action == NP_ACTION_BOOT)
        return valid_root(plan->root) ? 0 : -1;
    if (!valid_token(plan->disk_identifier, 20) ||
        !valid_token(plan->payload_tag, sizeof(plan->payload_tag) - 1) ||
        !valid_payload_path(plan->adapter) || !valid_payload_path(plan->source))
        return -1;
    return !plan->root[0] || valid_root(plan->root) ? 0 : -1;
}

static int receive_payload(int connection, uint8_t **payload, size_t *payload_length) {
    uint8_t header[NP_FRAME_HEADER_SIZE];
    if (read_full(connection, header, sizeof(header)) < 0 ||
        memcmp(header, "NPIP", 4) != 0 || header[4] != 1 || header[5] ||
        header[6] || header[7])
        return -1;
    uint32_t length = read_le32(header + 8);
    if (length < 12 || length > NP_MAX_PAYLOAD)
        return -1;
    uint8_t *bytes = malloc(length);
    if (!bytes)
        return -1;
    if (read_full(connection, bytes, length) < 0) {
        free(bytes);
        return -1;
    }
    *payload = bytes;
    *payload_length = length;
    return 0;
}

static int reserve(struct buffer *buffer, size_t extra) {
    if (extra > NP_MAX_PAYLOAD - buffer->length) {
        errno = EFBIG;
        return -1;
    }
    size_t required = buffer->length + extra;
    if (required <= buffer->capacity)
        return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < required)
        capacity = capacity > NP_MAX_PAYLOAD / 2 ? NP_MAX_PAYLOAD : capacity * 2;
    uint8_t *bytes = realloc(buffer->bytes, capacity);
    if (!bytes)
        return -1;
    buffer->bytes = bytes;
    buffer->capacity = capacity;
    return 0;
}

static int append(struct buffer *buffer, const void *bytes, size_t length) {
    if (reserve(buffer, length) < 0)
        return -1;
    memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length += length;
    return 0;
}

static int append_u8(struct buffer *buffer, uint8_t value) {
    return append(buffer, &value, 1);
}

static int append_u16(struct buffer *buffer, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return append(buffer, bytes, 2);
}

static int append_u32(struct buffer *buffer, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned index = 0; index < 4; index++)
        bytes[index] = (uint8_t)(value >> (index * 8));
    return append(buffer, bytes, 4);
}

static int append_u64(struct buffer *buffer, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned index = 0; index < 8; index++)
        bytes[index] = (uint8_t)(value >> (index * 8));
    return append(buffer, bytes, 8);
}

static int append_string(struct buffer *buffer, const char *value) {
    size_t length = strlen(value);
    if (length > UINT16_MAX || append_u16(buffer, (uint16_t)length) < 0)
        return -1;
    return append(buffer, value, length);
}

static int send_buffer(int connection, const struct buffer *payload) {
    uint8_t header[12] = {'N', 'P', 'I', 'P', 1, 0, 0, 0};
    uint32_t length = (uint32_t)payload->length;
    for (unsigned index = 0; index < 4; index++)
        header[8 + index] = (uint8_t)(length >> (index * 8));
    return write_full(connection, header, sizeof(header)) < 0 ||
                   write_full(connection, payload->bytes, payload->length) < 0
               ? -1 : 0;
}

static int send_ack(int connection, uint64_t request_id) {
    struct buffer response = {0};
    int result = append(&response, "NPOK", 4) < 0 ||
                         append_u64(&response, request_id) < 0
                     ? -1 : send_buffer(connection, &response);
    free(response.bytes);
    return result;
}

static int send_error(int connection, uint64_t request_id, int code, const char *message) {
    struct buffer response = {0};
    int result = append(&response, "NPER", 4) < 0 ||
                         append_u64(&response, request_id) < 0 ||
                         append_u32(&response, (uint32_t)code) < 0 ||
                         append_string(&response, message) < 0
                     ? -1 : send_buffer(connection, &response);
    free(response.bytes);
    return result;
}

static int connect_to_host(void) {
    int connection = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connection < 0)
        return -1;
    struct sockaddr_vm address;
    memset(&address, 0, sizeof(address));
    address.svm_family = AF_VSOCK;
    address.svm_cid = VMADDR_CID_HOST;
    address.svm_port = NP_INIT_PORT;
    if (connect(connection, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(connection);
        return -1;
    }
    return connection;
}

static int run(char *const arguments[], char *const environment[]) {
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execve(arguments[0], arguments, environment ? environment : environ);
        log_errno(arguments[0]);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    /* PID 1 also owns package-manager daemon children after they exit. */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_capture_line(char *const arguments[], char *output, size_t capacity) {
    int descriptors[2];
    if (capacity < 2 || pipe2(descriptors, O_CLOEXEC) < 0)
        return -1;
    pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(descriptors[1]);
        execv(arguments[0], arguments);
        _exit(127);
    }
    close(descriptors[1]);
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        char bytes[256];
        ssize_t count = read(descriptors[0], bytes, sizeof(bytes));
        if (count > 0) {
            size_t available = capacity - 1 - used;
            size_t copy = (size_t)count < available ? (size_t)count : available;
            memcpy(output + used, bytes, copy);
            used += copy;
            overflow |= copy != (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            overflow = true;
        break;
    }
    close(descriptors[0]);
    output[used] = '\0';

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (overflow) {
        errno = EOVERFLOW;
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = ENODEV;
        return -1;
    }
    output[strcspn(output, "\r\n")] = '\0';
    if (!output[0]) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

static int resolve_root_device(const char *root, char *device, size_t capacity) {
    if (strncmp(root, "/dev/", 5) == 0) {
        if (snprintf(device, capacity, "%s", root) >= (int)capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    char *const arguments[] = {
        "/sbin/blkid", "-l", "-t", (char *)root, "-o", "device", NULL,
    };
    if (run_capture_line(arguments, device, capacity) < 0)
        return -1;
    if (strncmp(device, "/dev/", 5) != 0 || !valid_root(device)) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

static int resolve_identifier_name(const char *identifier, char *name, size_t capacity);

static int resolve_disk_identifier(struct np_plan *plan) {
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        char name[sizeof(plan->disk) - sizeof("/dev/") + 1];
        if (resolve_identifier_name(plan->disk_identifier, name, sizeof(name)) == 0) {
            snprintf(plan->disk, sizeof(plan->disk), "/dev/%s", name);
            return 0;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
    errno = ENODEV;
    return -1;
}

static int mount_payload(const struct np_plan *plan) {
    if (umount2(NP_PAYLOAD_ROOT, 0) < 0 && errno != EINVAL && errno != ENOENT)
        return -1;
    return mount_once(plan->payload_tag, NP_PAYLOAD_ROOT, "virtiofs",
                      MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
}

static int run_adapter(const struct np_plan *plan, const char *action) {
    char disk[sizeof(plan->disk) + 16];
    char source[sizeof(plan->source) + 18];
    snprintf(disk, sizeof(disk), "NP_TARGET_DISK=%s", plan->disk);
    snprintf(source, sizeof(source), "NP_SOURCE_PATH=%s", plan->source);
    char *const environment[] = {
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin", "HOME=/", "TERM=linux",
        "NP_TARGET_ROOT=" NP_NEW_ROOT, disk, source,
        plan->automatic ? "NP_AUTOMATIC=1" : "NP_AUTOMATIC=0", NULL,
    };
    char *const arguments[] = {
        "/bin/sh", (char *)plan->adapter, (char *)action, NULL,
    };
    return run(arguments, environment);
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

static int read_text(const char *path, char *value, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t count = read(fd, value, capacity - 1);
    int saved = errno;
    close(fd);
    if (count < 0) {
        errno = saved;
        return -1;
    }
    value[count] = '\0';
    value[strcspn(value, "\r\n")] = '\0';
    return 0;
}

static int resolve_identifier_name(const char *identifier, char *name, size_t capacity) {
    DIR *directory = opendir("/sys/class/block");
    if (!directory)
        return -1;
    int result = -1;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        char path[512], serial[64];
        snprintf(path, sizeof(path), "/sys/class/block/%s/serial", entry->d_name);
        if (read_text(path, serial, sizeof(serial)) == 0 && !strcmp(serial, identifier)) {
            if (snprintf(name, capacity, "%s", entry->d_name) < (int)capacity)
                result = 0;
            break;
        }
    }
    closedir(directory);
    if (result < 0)
        errno = ENODEV;
    return result;
}

static int send_inventory(int connection, uint64_t request_id) {
    struct buffer response = {0};
    if (append(&response, "NPIB", 4) < 0 || append_u64(&response, request_id) < 0 ||
        append_u16(&response, 0) < 0)
        goto failure;
    uint16_t count = 0;
    DIR *directory = opendir("/sys/class/block");
    if (!directory)
        goto failure;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.')
            continue;
        char path[512], identifier[64] = "", value[64] = "0";
        snprintf(path, sizeof(path), "/sys/class/block/%s/serial", entry->d_name);
        read_text(path, identifier, sizeof(identifier));
        snprintf(path, sizeof(path), "/sys/class/block/%s/size", entry->d_name);
        read_text(path, value, sizeof(value));
        uint64_t size = strtoull(value, NULL, 10) * 512u;
        snprintf(path, sizeof(path), "/sys/class/block/%s/ro", entry->d_name);
        bool read_only = read_text(path, value, sizeof(value)) < 0 || value[0] != '0';
        snprintf(path, sizeof(path), "/sys/class/block/%s/partition", entry->d_name);
        bool partition = access(path, F_OK) == 0;
        if (append_string(&response, entry->d_name) < 0 ||
            append_string(&response, identifier) < 0 || append_u64(&response, size) < 0 ||
            append_u8(&response, read_only) < 0 || append_u8(&response, partition) < 0) {
            closedir(directory);
            goto failure;
        }
        count++;
    }
    closedir(directory);
    response.bytes[12] = (uint8_t)count;
    response.bytes[13] = (uint8_t)(count >> 8);
    int result = send_buffer(connection, &response);
    free(response.bytes);
    return result;
failure:
    free(response.bytes);
    return -1;
}

static int mount_selected(const char *identifier, uint16_t partition, bool writable) {
    char name[64], device[128], sysfs[256];
    if (!valid_token(identifier, 20) ||
        resolve_identifier_name(identifier, name, sizeof(name)) < 0)
        return -1;
    if (partition) {
        const char *separator = isdigit((unsigned char)name[strlen(name) - 1]) ? "p" : "";
        snprintf(device, sizeof(device), "/dev/%s%s%u", name, separator, partition);
        snprintf(sysfs, sizeof(sysfs), "/sys/class/block/%s%s%u/partition", name, separator, partition);
        if (access(sysfs, F_OK) < 0)
            return -1;
    } else {
        snprintf(device, sizeof(device), "/dev/%s", name);
    }
    if (root_is_mounted()) {
        char *const unmount_arguments[] = {"/bin/umount", NP_NEW_ROOT, NULL};
        if (run(unmount_arguments, NULL) != 0) {
            errno = EBUSY;
            return -1;
        }
    }
    char *const arguments[] = {
        "/bin/mount", "-o", writable ? "rw" : "ro", device, NP_NEW_ROOT, NULL,
    };
    if (run(arguments, NULL) != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int open_root(void) {
    if (!root_is_mounted()) {
        errno = ENOMEDIUM;
        return -1;
    }
    return open(NP_NEW_ROOT, O_PATH | O_DIRECTORY | O_CLOEXEC);
}

static const char *relative_path(const char *path) {
    while (*path == '/')
        path++;
    return *path ? path : ".";
}

static int open_in_target_root(int root, const char *path, int flags, mode_t mode) {
    struct open_how how = {
        .flags = (uint64_t)flags,
        .mode = mode,
        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS,
    };
    return (int)syscall(SYS_openat2, root, relative_path(path), &how, sizeof(how));
}

static int read_request_path(const uint8_t *payload, size_t length, char path[4096]) {
    struct reader reader = {payload, length, 12};
    if (take_string(&reader, path, 4096) < 0 || reader.offset != length || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int send_path_stat(int connection, uint64_t request_id, const char *path) {
    int root = open_root();
    if (root < 0)
        return -1;
    int fd = open_in_target_root(root, path, O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
    close(root);
    if (fd < 0)
        return -1;
    struct stat status;
    int result = fstat(fd, &status);
    close(fd);
    if (result < 0)
        return -1;
    struct buffer response = {0};
    result = append(&response, "NPFS", 4) < 0 ||
                     append_u64(&response, request_id) < 0 ||
                     append_string(&response, path) < 0 ||
                     append_u32(&response, (uint32_t)status.st_mode) < 0 ||
                     append_u32(&response, (uint32_t)status.st_uid) < 0 ||
                     append_u32(&response, (uint32_t)status.st_gid) < 0 ||
                     append_u64(&response, (uint64_t)status.st_size) < 0 ||
                     append_u64(&response, (uint64_t)status.st_mtime) < 0
                 ? -1 : send_buffer(connection, &response);
    free(response.bytes);
    return result;
}

static int send_path_contents(int connection, uint64_t request_id, const char *path) {
    int root = open_root();
    if (root < 0)
        return -1;
    int fd = open_in_target_root(root, path, O_RDONLY | O_CLOEXEC, 0);
    close(root);
    if (fd < 0)
        return -1;
    struct stat status;
    if (fstat(fd, &status) < 0) {
        close(fd);
        return -1;
    }
    struct buffer response = {0};
    int result = -1;
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0 || (uint64_t)status.st_size > NP_MAX_PAYLOAD - 32u - strlen(path)) {
            errno = EFBIG;
            goto done;
        }
        if (append(&response, "NPFL", 4) < 0 || append_u64(&response, request_id) < 0 ||
            append_string(&response, path) < 0 ||
            append_u64(&response, (uint64_t)status.st_size) < 0 ||
            reserve(&response, (size_t)status.st_size) < 0)
            goto done;
        if (read_full(fd, response.bytes + response.length, (size_t)status.st_size) < 0)
            goto done;
        response.length += (size_t)status.st_size;
    } else if (S_ISDIR(status.st_mode)) {
        DIR *directory = fdopendir(fd);
        if (!directory)
            goto done;
        fd = -1;
        if (append(&response, "NPLS", 4) < 0 || append_u64(&response, request_id) < 0 ||
            append_string(&response, path) < 0 || append_u32(&response, 0) < 0 ||
            append_u32(&response, 0) < 0) {
            closedir(directory);
            goto done;
        }
        uint32_t count = 0;
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            if (append_u8(&response, entry->d_type) < 0 || append_u8(&response, 0) < 0 ||
                append_string(&response, entry->d_name) < 0) {
                closedir(directory);
                goto done;
            }
            count++;
        }
        closedir(directory);
        size_t count_offset = 4 + 8 + 2 + strlen(path) + 4;
        for (unsigned index = 0; index < 4; index++)
            response.bytes[count_offset + index] = (uint8_t)(count >> (index * 8));
    } else {
        errno = EINVAL;
        goto done;
    }
    result = send_buffer(connection, &response);
done:
    if (fd >= 0)
        close(fd);
    free(response.bytes);
    return result;
}

static int write_path(const uint8_t *payload, size_t length) {
    struct reader reader = {payload, length, 12};
    char path[4096];
    uint8_t fixed[12];
    if (take_string(&reader, path, sizeof(path)) < 0 || path[0] != '/' ||
        take_bytes(&reader, fixed, sizeof(fixed)) < 0) {
        errno = EINVAL;
        return -1;
    }
    uint32_t mode = read_le32(fixed);
    uint64_t size = read_le64(fixed + 4);
#if SIZE_MAX < UINT64_MAX
    if (size > SIZE_MAX) {
        errno = EFBIG;
        return -1;
    }
#endif
    if ((size_t)size != reader.length - reader.offset) {
        errno = EINVAL;
        return -1;
    }
    int root = open_root();
    if (root < 0)
        return -1;
    int fd = open_in_target_root(root, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          (mode_t)(mode & 07777u));
    close(root);
    if (fd < 0)
        return -1;
    int result = write_full(fd, reader.bytes + reader.offset, (size_t)size);
    if (result == 0)
        result = fchmod(fd, (mode_t)(mode & 07777u));
    close(fd);
    return result;
}

static void sleep_milliseconds(uint64_t milliseconds) {
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)((milliseconds % 1000) * 1000000),
    };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int mount_root(const struct np_plan *plan) {
    if (root_is_mounted()) {
        /* An adapter may deliberately leave its root mounted and omit root=.
         * An explicit root= is different: always remount that exact device so
         * a failed earlier attempt cannot make a later boot use stale state. */
        if (!plan->root[0])
            return 0;
        if (umount2(NP_NEW_ROOT, 0) < 0)
            return -1;
    }
    if (!valid_root(plan->root)) {
        errno = EINVAL;
        return -1;
    }
    if (plan->root_delay_seconds)
        sleep_milliseconds((uint64_t)plan->root_delay_seconds * 1000);

    const uint64_t started = monotonic_milliseconds();
    const uint64_t limit = plan->root_wait
        ? (plan->root_wait_seconds ? (uint64_t)plan->root_wait_seconds * 1000 : UINT64_MAX)
        : NP_DEFAULT_ROOT_WAIT_MILLISECONDS;
    for (;;) {
        char device[256];
        struct stat device_status;
        if (resolve_root_device(plan->root, device, sizeof(device)) == 0 &&
            stat(device, &device_status) == 0 && S_ISBLK(device_status.st_mode)) {
            char options[sizeof(plan->root_flags) + 4];
            int option_length = snprintf(
                options, sizeof(options), "%s%s%s",
                plan->root_read_only ? "ro" : "rw",
                plan->root_flags[0] ? "," : "", plan->root_flags);
            if (option_length < 0 || (size_t)option_length >= sizeof(options)) {
                errno = EOVERFLOW;
                return -1;
            }
            char *arguments[9];
            size_t index = 0;
            arguments[index++] = "/bin/mount";
            if (plan->root_fstype[0]) {
                arguments[index++] = "-t";
                arguments[index++] = (char *)plan->root_fstype;
            }
            arguments[index++] = "-o";
            arguments[index++] = options;
            arguments[index++] = device;
            arguments[index++] = NP_NEW_ROOT;
            arguments[index] = NULL;
            if (run(arguments, NULL) == 0)
                return 0;
            /* rootwait waits for the device, not for a broken filesystem to
             * repair itself. Return to the host's recovery channel on failure. */
            errno = EIO;
            return -1;
        }

        uint64_t now = monotonic_milliseconds();
        if (limit != UINT64_MAX && now - started >= limit)
            break;
        sleep_milliseconds(100);
    }
    errno = ENODEV;
    return -1;
}

static int preflight_executable_at(int root, const char *path, unsigned depth) {
    if (!valid_init(path)) {
        errno = EINVAL;
        return -1;
    }
    if (depth > 2) {
        errno = ELOOP;
        return -1;
    }
    int fd = open_in_target_root(root, path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct stat status;
    if (fstat(fd, &status) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(status.st_mode) ||
        !(status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        close(fd);
        errno = EACCES;
        return -1;
    }

    uint8_t header[sizeof(Elf64_Ehdr)];
    ssize_t count = pread(fd, header, sizeof(header), 0);
    if (count >= 2 && header[0] == '#' && header[1] == '!') {
        char line[256];
        count = pread(fd, line, sizeof(line) - 1, 0);
        close(fd);
        if (count < 3) {
            errno = ENOEXEC;
            return -1;
        }
        line[count] = '\0';
        char *interpreter = line + 2;
        while (*interpreter == ' ' || *interpreter == '\t')
            interpreter++;
        char *end = interpreter;
        while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
            end++;
        *end = '\0';
        return preflight_executable_at(root, interpreter, depth + 1);
    }
    if (count != (ssize_t)sizeof(header) || memcmp(header, ELFMAG, SELFMAG) != 0 ||
        header[EI_CLASS] != ELFCLASS64 || header[EI_DATA] != ELFDATA2LSB ||
        header[EI_VERSION] != EV_CURRENT) {
        close(fd);
        errno = ENOEXEC;
        return -1;
    }

    Elf64_Ehdr executable;
    memcpy(&executable, header, sizeof(executable));
    if ((executable.e_type != ET_EXEC && executable.e_type != ET_DYN) ||
        executable.e_machine != NP_ELF_MACHINE || executable.e_version != EV_CURRENT ||
        executable.e_ehsize != sizeof(Elf64_Ehdr) ||
        executable.e_phentsize != sizeof(Elf64_Phdr) || !executable.e_phnum ||
        executable.e_phnum > 128 || executable.e_phoff > (uint64_t)status.st_size) {
        close(fd);
        errno = ENOEXEC;
        return -1;
    }
    bool has_load_segment = false;
    char interpreter[256] = {0};
    for (Elf64_Half index = 0; index < executable.e_phnum; index++) {
        uint64_t relative_offset = (uint64_t)index * sizeof(Elf64_Phdr);
        if (relative_offset > (uint64_t)status.st_size - executable.e_phoff ||
            sizeof(Elf64_Phdr) >
                (uint64_t)status.st_size - executable.e_phoff - relative_offset) {
            close(fd);
            errno = ENOEXEC;
            return -1;
        }
        uint64_t offset = executable.e_phoff + relative_offset;
        Elf64_Phdr segment;
        if (pread(fd, &segment, sizeof(segment), (off_t)offset) != (ssize_t)sizeof(segment)) {
            int saved = errno ? errno : EIO;
            close(fd);
            errno = saved;
            return -1;
        }
        if (segment.p_type == PT_LOAD) {
            if (segment.p_filesz > segment.p_memsz ||
                segment.p_offset > (uint64_t)status.st_size ||
                segment.p_filesz > (uint64_t)status.st_size - segment.p_offset) {
                close(fd);
                errno = ENOEXEC;
                return -1;
            }
            has_load_segment = true;
            continue;
        }
        if (segment.p_type != PT_INTERP)
            continue;
        if (interpreter[0] || segment.p_filesz < 2 ||
            segment.p_filesz > sizeof(interpreter) ||
            segment.p_offset > (uint64_t)status.st_size ||
            segment.p_filesz > (uint64_t)status.st_size - segment.p_offset ||
            pread(fd, interpreter, (size_t)segment.p_filesz, (off_t)segment.p_offset) !=
                (ssize_t)segment.p_filesz ||
            interpreter[segment.p_filesz - 1] != '\0') {
            close(fd);
            errno = ENOEXEC;
            return -1;
        }
    }
    close(fd);
    if (!has_load_segment) {
        errno = ENOEXEC;
        return -1;
    }
    return interpreter[0] ? preflight_executable_at(root, interpreter, depth + 1) : 0;
}

static int preflight_target_init(const struct np_plan *plan) {
    int root = open(NP_NEW_ROOT, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root < 0)
        return -1;
    int result = preflight_executable_at(root, plan->init, 0);
    close(root);
    return result;
}

static int preflight_switch_root(void) {
    int root = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root < 0)
        return -1;
    int result = preflight_executable_at(root, "/sbin/switch_root", 0);
    close(root);
    return result;
}

struct runtime_mount {
    const char *source;
    const char *target;
    mode_t mode;
};

static const struct runtime_mount runtime_mounts[] = {
    {"/dev", NP_NEW_ROOT "/dev", 0755},
    {"/proc", NP_NEW_ROOT "/proc", 0555},
    {"/sys", NP_NEW_ROOT "/sys", 0555},
    {"/run", NP_NEW_ROOT "/run", 0755},
    {"/tmp", NP_NEW_ROOT "/tmp", 01777},
};

static int prepare_runtime_mounts(void) {
    for (size_t index = 0; index < sizeof(runtime_mounts) / sizeof(runtime_mounts[0]); index++) {
        const struct runtime_mount *item = &runtime_mounts[index];
        if (mkdir(item->target, item->mode) < 0 && errno != EEXIST)
            return -1;
        struct stat status;
        if (lstat(item->target, &status) < 0 || !S_ISDIR(status.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
    }
    return 0;
}

static void rollback_runtime_mounts(size_t moved) {
    while (moved) {
        const struct runtime_mount *item = &runtime_mounts[--moved];
        if (mount(item->target, item->source, NULL, MS_MOVE, NULL) < 0)
            log_errno("rollback runtime mount");
    }
}

static int move_runtime_mounts(size_t *moved) {
    *moved = 0;
    for (size_t index = 0; index < sizeof(runtime_mounts) / sizeof(runtime_mounts[0]); index++) {
        const struct runtime_mount *item = &runtime_mounts[index];
        if (mount(item->source, item->target, NULL, MS_MOVE, NULL) < 0) {
            int saved = errno;
            rollback_runtime_mounts(*moved);
            errno = saved;
            return -1;
        }
        (*moved)++;
    }
    return 0;
}

static int switch_to_root(
    const struct np_plan *plan, int acknowledgement_connection, bool *response_sent
) {
    if (response_sent)
        *response_sent = false;
    if (mount_root(plan) < 0 || preflight_target_init(plan) < 0 ||
        preflight_switch_root() < 0 || prepare_runtime_mounts() < 0)
        return -1;
    size_t moved = 0;
    if (move_runtime_mounts(&moved) < 0)
        return -1;
    if (acknowledgement_connection >= 0) {
        if (send_ack(acknowledgement_connection, plan->request_id) < 0) {
            rollback_runtime_mounts(moved);
            return -1;
        }
        if (response_sent)
            *response_sent = true;
    }

    close_control_transport();
    char *const arguments[] = {
        "/sbin/switch_root", "-c", "/dev/console", NP_NEW_ROOT, (char *)plan->init, NULL,
    };
    execv(arguments[0], arguments);
    int saved = errno;
    rollback_runtime_mounts(moved);
    errno = saved;
    log_errno("exec switch_root");
    return -1;
}

static int prepare_plan(struct np_plan *plan) {
    if (plan->action == NP_ACTION_INSTALL || plan->action == NP_ACTION_REPAIR) {
        if (resolve_disk_identifier(plan) < 0) {
            log_errno("target disk identifier not found");
            return -1;
        }
        if (mount_payload(plan) < 0)
            return -1;
        const char *action = plan->action == NP_ACTION_INSTALL ? "install" : "repair";
        if (run_adapter(plan, action) != 0) {
            logmsg("adapter failed; waiting for another host command");
            errno = EIO;
            return -1;
        }
    }
    return 0;
}

static int execute_plan(struct np_plan *plan, int connection, bool *response_sent) {
    *response_sent = false;
    if (plan->action == NP_ACTION_SHELL) {
        if (send_ack(connection, plan->request_id) < 0)
            return -1;
        *response_sent = true;
        char *const arguments[] = {"/bin/sh", "-l", NULL};
        return run(arguments, NULL);
    }
    if (prepare_plan(plan) < 0)
        return -1;
    if (switch_to_root(plan, connection, response_sent) < 0) {
        logmsg("root handoff failed; waiting for another host command");
        return -1;
    }
    return 0;
}

static int append_guest_info(struct buffer *response) {
    struct utsname system;
    const char *release = uname(&system) == 0 ? system.release : "";
    const char *fields[] = {
        "initramfs-1", release, "LightHouse Recovery", "1", "nativepipe-init",
    };
    const char *capabilities[] = {
        "init.control", "init.mount", "init.execute", "init.install.rootfs.v1",
        "fs.read", "fs.stat", "fs.write",
    };
    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); index++) {
        if (append_string(response, fields[index]) < 0)
            return -1;
    }
    if (append_u16(response, (uint16_t)(sizeof(capabilities) / sizeof(capabilities[0]))) < 0)
        return -1;
    for (size_t index = 0; index < sizeof(capabilities) / sizeof(capabilities[0]); index++) {
        if (append_string(response, capabilities[index]) < 0)
            return -1;
    }
    return 0;
}

static int send_hello(int connection, uint64_t request_id) {
    struct buffer response = {0};
    if (append(&response, "NPIF", 4) < 0 || append_u64(&response, request_id) < 0 ||
        append_guest_info(&response) < 0)
        goto failure;
    int result = send_buffer(connection, &response);
    free(response.bytes);
    return result;
failure:
    free(response.bytes);
    return -1;
}

/* Recovery is guest-initiated.  NPRT is the existing unsolicited ready
 * handshake, so the accepted socket immediately becomes the ordinary NPIP
 * control channel; no recovery-only protocol or second negotiation is needed. */
static int send_ready_handshake(int connection) {
    struct buffer event = {0};
    if (append(&event, "NPRT", 4) < 0 || append_guest_info(&event) < 0) {
        free(event.bytes);
        return -1;
    }
    int result = send_buffer(connection, &event);
    free(event.bytes);
    return result;
}

static int dispatch_request(int connection, const uint8_t *payload, size_t length) {
    if (length < 12) {
        errno = EINVAL;
        return -1;
    }
    uint64_t request_id = read_le64(payload + 4);
    int result = -1;
    if (!memcmp(payload, "NPHI", 4)) {
        result = send_hello(connection, request_id);
    } else if (!memcmp(payload, "NPPG", 4) && length == 12) {
        result = send_ack(connection, request_id);
    } else if (!memcmp(payload, "NPIH", 4) && length == 12) {
        result = send_inventory(connection, request_id);
    } else if (!memcmp(payload, "NPIM", 4)) {
        struct reader reader = {payload, length, 12};
        char identifier[21];
        uint8_t fixed[4];
        if (take_string(&reader, identifier, sizeof(identifier)) == 0 &&
            take_bytes(&reader, fixed, sizeof(fixed)) == 0 && reader.offset == length &&
            fixed[3] == 0 &&
            mount_selected(identifier, read_le16(fixed), fixed[2] != 0) == 0)
            result = send_ack(connection, request_id);
        else if (!errno)
            errno = EINVAL;
    } else if (!memcmp(payload, "NPRE", 4)) {
        char path[4096];
        if (read_request_path(payload, length, path) == 0)
            result = send_path_contents(connection, request_id, path);
    } else if (!memcmp(payload, "NPMS", 4)) {
        char path[4096];
        if (read_request_path(payload, length, path) == 0)
            result = send_path_stat(connection, request_id, path);
    } else if (!memcmp(payload, "NPWR", 4)) {
        if (write_path(payload, length) == 0)
            result = send_ack(connection, request_id);
    } else if (!memcmp(payload, "NPIC", 4)) {
        struct np_plan plan;
        if (decode_plan(payload, length, &plan) == 0) {
            bool response_sent = false;
            result = execute_plan(&plan, connection, &response_sent);
            if (result < 0) {
                int code = errno ? errno : EIO;
                log_errno("execute plan");
                if (!response_sent)
                    send_error(connection, request_id, code, strerror(code));
            }
            return result;
        }
        errno = EINVAL;
    } else {
        errno = ENOSYS;
    }
    if (result < 0) {
        int code = errno ? errno : EINVAL;
        send_error(connection, request_id, code, strerror(code));
    }
    return result;
}

static int copy_parameter(char *destination, size_t capacity, const char *value) {
    size_t length = strlen(value);
    if (!length || length >= capacity) {
        errno = EINVAL;
        return -1;
    }
    memcpy(destination, value, length + 1);
    return 0;
}

static int parse_seconds(const char *value, uint32_t *seconds, bool allow_zero) {
    if (*value < '0' || *value > '9') {
        errno = EINVAL;
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed > UINT32_MAX || (!allow_zero && parsed == 0)) {
        errno = EINVAL;
        return -1;
    }
    *seconds = (uint32_t)parsed;
    return 0;
}

static int parse_boot_configuration(char *command_line, bool *maintenance,
                                    struct np_plan *plan, uint64_t *target_memory) {
    *maintenance = false;
    *target_memory = 0;
    initialize_plan(plan);
    char *save = NULL;
    for (char *token = strtok_r(command_line, " \t\r\n", &save); token;
         token = strtok_r(NULL, " \t\r\n", &save)) {
        if (!strcmp(token, "nativepipe.maintenance=1")) {
            *maintenance = true;
        } else if (!strncmp(token, "root=", 5)) {
            if (copy_parameter(plan->root, sizeof(plan->root), token + 5) < 0)
                return -1;
        } else if (!strncmp(token, "rootfstype=", 11)) {
            if (copy_parameter(
                    plan->root_fstype, sizeof(plan->root_fstype), token + 11) < 0)
                return -1;
        } else if (!strncmp(token, "rootflags=", 10)) {
            if (copy_parameter(plan->root_flags, sizeof(plan->root_flags), token + 10) < 0)
                return -1;
        } else if (!strcmp(token, "ro")) {
            plan->root_read_only = true;
        } else if (!strcmp(token, "rw")) {
            plan->root_read_only = false;
        } else if (!strcmp(token, "rootwait")) {
            plan->root_wait = true;
            plan->root_wait_seconds = 0;
        } else if (!strncmp(token, "rootwait=", 9)) {
            plan->root_wait = true;
            if (parse_seconds(token + 9, &plan->root_wait_seconds, false) < 0)
                return -1;
        } else if (!strncmp(token, "rootdelay=", 10)) {
            if (parse_seconds(token + 10, &plan->root_delay_seconds, true) < 0)
                return -1;
        } else if (!strncmp(token, "init=", 5)) {
            if (copy_parameter(plan->init, sizeof(plan->init), token + 5) < 0)
                return -1;
        } else if (!strncmp(token, "nativepipe.memory_target_bytes=", 31)) {
            const char *value = token + 31;
            if (*value < '0' || *value > '9') {
                errno = EINVAL;
                return -1;
            }
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(value, &end, 10);
            if (errno || !end || *end || parsed == 0 ||
                parsed % NP_MEMORY_ALIGNMENT_BYTES != 0) {
                errno = EINVAL;
                return -1;
            }
            *target_memory = (uint64_t)parsed;
        }
    }
    if ((plan->root[0] && !valid_root(plan->root)) ||
        (plan->root_fstype[0] && !valid_fstype(plan->root_fstype)) ||
        !valid_root_flags(plan->root_flags) || !valid_init(plan->init)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int read_boot_configuration(bool *maintenance, struct np_plan *plan,
                                   uint64_t *target_memory) {
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    char command_line[4096];
    ssize_t count = read(fd, command_line, sizeof(command_line) - 1);
    int saved = errno;
    close(fd);
    if (count < 0) {
        errno = saved;
        return -1;
    }
    command_line[count] = '\0';
    return parse_boot_configuration(command_line, maintenance, plan, target_memory);
}

int main(void) {
    if (getpid() != 1) {
        logmsg("must run as PID 1");
        return 1;
    }
    bool runtime_failed = setup_runtime() < 0;
    if (runtime_failed)
        log_errno("initialize recovery runtime");
    bool maintenance = false;
    uint64_t target_memory = 0;
    struct np_plan boot_plan;
    initialize_plan(&boot_plan);
    if (read_boot_configuration(&maintenance, &boot_plan, &target_memory) < 0) {
        log_errno("read kernel command line");
        maintenance = true;
    }
    maintenance |= runtime_failed;
    if (target_memory && publish_target_memory(target_memory) < 0)
        log_errno("publish target memory");
    if (!maintenance) {
        if (!valid_root(boot_plan.root)) {
            logmsg("normal boot requires an explicit root= kernel argument");
            errno = EINVAL;
        } else {
            logmsg("normal boot; switching directly to the configured root");
            if (switch_to_root(&boot_plan, -1, NULL) < 0)
                log_errno("normal root handoff");
        }
        logmsg("normal boot could not hand off; entering recovery");
    } else {
        logmsg("recovery requested by the kernel command line");
    }

    logmsg("recovery; connecting to host control on vsock CID 2 port 1024");
    unsigned failed_connections = 0;
    for (;;) {
        control_connection = connect_to_host();
        if (control_connection < 0) {
            if (failed_connections++ % 30 == 0)
                log_errno("vsock connect to host");
            struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};
            nanosleep(&delay, NULL);
            continue;
        }
        failed_connections = 0;
        if (send_ready_handshake(control_connection) < 0) {
            log_errno("send recovery handshake");
            close_control_transport();
            sleep_milliseconds(1000);
            continue;
        }
        logmsg("recovery control connected");
        for (;;) {
            uint8_t *payload = NULL;
            size_t length = 0;
            if (receive_payload(control_connection, &payload, &length) < 0) {
                free(payload);
                break;
            }
            dispatch_request(control_connection, payload, length);
            free(payload);
        }
        close_control_transport();
        sleep_milliseconds(1000);
    }
}
