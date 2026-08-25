#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
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
    char disk_identifier[21];
    char disk[256];
    char root[256];
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

static int control_listener = -1;
static int control_connection = -1;

static void close_control_transport(void) {
    if (control_connection >= 0) {
        shutdown(control_connection, SHUT_RDWR);
        close(control_connection);
        control_connection = -1;
    }
    if (control_listener >= 0) {
        close(control_listener);
        control_listener = -1;
    }
}

static void logmsg(const char *message) {
    dprintf(STDERR_FILENO, "[nativepipe-init] %s\n", message);
}

static void log_errno(const char *operation) {
    dprintf(STDERR_FILENO, "[nativepipe-init] %s: %s\n", operation, strerror(errno));
}

static void make_directory(const char *path, mode_t mode) {
    if (mkdir(path, mode) < 0 && errno != EEXIST) {
        log_errno(path);
        _exit(1);
    }
}

static int mount_once(const char *source, const char *target, const char *filesystem,
                      unsigned long flags, const char *data) {
    if (mount(source, target, filesystem, flags, data) == 0 || errno == EBUSY)
        return 0;
    log_errno(target);
    return -1;
}

static void setup_runtime(void) {
    umask(022);
    signal(SIGPIPE, SIG_IGN);
    make_directory("/dev", 0755);
    make_directory("/proc", 0555);
    make_directory("/sys", 0555);
    make_directory("/run", 0755);
    make_directory("/tmp", 01777);
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
    mount_once("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    make_directory("/run/nativepipe", 0755);
    make_directory(NP_PAYLOAD_ROOT, 0755);
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
    if (length >= capacity || length > reader->length - reader->offset)
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

static bool valid_root(const char *root) {
    return (strncmp(root, "/dev/", 5) == 0 && !has_parent_component(root)) ||
           strncmp(root, "UUID=", 5) == 0 || strncmp(root, "PARTUUID=", 9) == 0 ||
           strncmp(root, "LABEL=", 6) == 0;
}

static bool valid_payload_path(const char *path) {
    size_t prefix = strlen(NP_PAYLOAD_ROOT);
    return strncmp(path, NP_PAYLOAD_ROOT "/", prefix + 1) == 0 &&
           !has_parent_component(path);
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

static int decode_plan(const uint8_t *payload, size_t length, struct np_plan *plan) {
    memset(plan, 0, sizeof(*plan));
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

static int listen_for_host(void) {
    int listener = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
        return -1;
    struct sockaddr_vm address;
    memset(&address, 0, sizeof(address));
    address.svm_family = AF_VSOCK;
    address.svm_cid = VMADDR_CID_ANY;
    address.svm_port = NP_INIT_PORT;
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 4) < 0) {
        close(listener);
        return -1;
    }
    return listener;
}

static int run(char *const arguments[]) {
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execv(arguments[0], arguments);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int resolve_disk_identifier(struct np_plan *plan) {
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        DIR *directory = opendir("/sys/class/block");
        if (directory) {
            struct dirent *entry;
            while ((entry = readdir(directory))) {
                char serial_path[512];
                snprintf(serial_path, sizeof(serial_path), "/sys/class/block/%s/serial",
                         entry->d_name);
                FILE *serial = fopen(serial_path, "r");
                if (!serial)
                    continue;
                char value[64] = {0};
                if (fgets(value, sizeof(value), serial))
                    value[strcspn(value, "\r\n")] = '\0';
                fclose(serial);
                if (strcmp(value, plan->disk_identifier) == 0) {
                    size_t name_length = strnlen(entry->d_name, sizeof(entry->d_name));
                    if (name_length > sizeof(plan->disk) - sizeof("/dev/"))
                        continue;
                    memcpy(plan->disk, "/dev/", sizeof("/dev/") - 1);
                    memcpy(plan->disk + sizeof("/dev/") - 1, entry->d_name,
                           name_length + 1);
                    closedir(directory);
                    return 0;
                }
            }
            closedir(directory);
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
    errno = ENODEV;
    return -1;
}

static int mount_payload(const struct np_plan *plan) {
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
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execve(arguments[0], arguments, environment);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
        snprintf(device, sizeof(device), "/dev/%s%u", name, partition);
        snprintf(sysfs, sizeof(sysfs), "/sys/class/block/%s%u/partition", name, partition);
        if (access(sysfs, F_OK) < 0)
            return -1;
    } else {
        snprintf(device, sizeof(device), "/dev/%s", name);
    }
    if (root_is_mounted()) {
        char *const unmount_arguments[] = {"/bin/umount", NP_NEW_ROOT, NULL};
        if (run(unmount_arguments) != 0) {
            errno = EBUSY;
            return -1;
        }
    }
    char *const arguments[] = {
        "/bin/mount", "-o", writable ? "rw" : "ro", device, NP_NEW_ROOT, NULL,
    };
    if (run(arguments) != 0) {
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

static int open_beneath(int root, const char *path, int flags, mode_t mode) {
    struct open_how how = {
        .flags = (uint64_t)flags,
        .mode = mode,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS,
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
    int fd = open_beneath(root, path, O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
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
    int fd = open_beneath(root, path, O_RDONLY | O_CLOEXEC, 0);
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
    int fd = open_beneath(root, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
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

static int mount_root(const struct np_plan *plan) {
    if (root_is_mounted())
        return 0;
    if (!plan->root[0]) {
        errno = ENOENT;
        return -1;
    }
    char *const arguments[] = {
        "/bin/mount", "-o", "rw", (char *)plan->root, NP_NEW_ROOT, NULL,
    };
    for (unsigned attempt = 0; attempt < 300; attempt++) {
        if (run(arguments) == 0)
            return 0;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
    errno = ENODEV;
    return -1;
}

static int switch_to_root(const struct np_plan *plan) {
    if (mount_root(plan) != 0 || access(NP_NEW_ROOT "/sbin/init", X_OK) < 0)
        return -1;
    make_directory(NP_NEW_ROOT "/dev", 0755);
    make_directory(NP_NEW_ROOT "/proc", 0555);
    make_directory(NP_NEW_ROOT "/sys", 0555);
    make_directory(NP_NEW_ROOT "/run", 0755);
    mount("/dev", NP_NEW_ROOT "/dev", NULL, MS_MOVE, NULL);
    mount("/proc", NP_NEW_ROOT "/proc", NULL, MS_MOVE, NULL);
    mount("/sys", NP_NEW_ROOT "/sys", NULL, MS_MOVE, NULL);
    mount("/run", NP_NEW_ROOT "/run", NULL, MS_MOVE, NULL);
    close_control_transport();
    char *const arguments[] = {
        "/sbin/switch_root", "-c", "/dev/console", NP_NEW_ROOT, "/sbin/init", NULL,
    };
    execv(arguments[0], arguments);
    log_errno("exec installed init");
    for (;;)
        pause();
}

static int execute_plan(struct np_plan *plan) {
    if (plan->action == NP_ACTION_SHELL) {
        char *const arguments[] = {"/bin/sh", "-l", NULL};
        return run(arguments);
    }
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
            return -1;
        }
    }
    if (switch_to_root(plan) < 0) {
        logmsg("root handoff failed; waiting for another host command");
        return -1;
    }
    return 0;
}

static int send_hello(int connection, uint64_t request_id) {
    struct utsname system;
    const char *release = uname(&system) == 0 ? system.release : "";
    const char *fields[] = {
        "initramfs-1", release, "LightHouse Recovery", "1", "nativepipe-init",
    };
    const char *capabilities[] = {
        "init.control", "init.mount", "init.execute", "fs.read", "fs.stat", "fs.write",
    };
    struct buffer response = {0};
    if (append(&response, "NPIF", 4) < 0 || append_u64(&response, request_id) < 0)
        goto failure;
    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); index++) {
        if (append_string(&response, fields[index]) < 0)
            goto failure;
    }
    if (append_u16(&response, (uint16_t)(sizeof(capabilities) / sizeof(capabilities[0]))) < 0)
        goto failure;
    for (size_t index = 0; index < sizeof(capabilities) / sizeof(capabilities[0]); index++) {
        if (append_string(&response, capabilities[index]) < 0)
            goto failure;
    }
    int result = send_buffer(connection, &response);
    free(response.bytes);
    return result;
failure:
    free(response.bytes);
    return -1;
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
            result = send_ack(connection, request_id);
            if (result == 0 && execute_plan(&plan) < 0)
                log_errno("execute plan");
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

static int parse_boot_configuration(char *command_line, bool *maintenance, char *root,
                                    size_t root_capacity, uint64_t *target_memory) {
    *maintenance = false;
    *target_memory = 0;
    root[0] = '\0';
    char *save = NULL;
    for (char *token = strtok_r(command_line, " \t\r\n", &save); token;
         token = strtok_r(NULL, " \t\r\n", &save)) {
        if (!strcmp(token, "nativepipe.maintenance=1")) {
            *maintenance = true;
        } else if (!strncmp(token, "root=", 5)) {
            size_t length = strlen(token + 5);
            if (!length || length >= root_capacity) {
                errno = EINVAL;
                return -1;
            }
            memcpy(root, token + 5, length + 1);
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
    return 0;
}

static int self_test(void) {
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
        strcmp(plan.disk_identifier, "nativepipe-root") != 0)
        return 1;

    char valid[] = "console=hvc0 root=/dev/vda2 nativepipe.memory_target_bytes=2147483648";
    char root[256];
    bool maintenance = false;
    uint64_t target_memory = 0;
    if (parse_boot_configuration(valid, &maintenance, root, sizeof(root), &target_memory) < 0 ||
        maintenance || strcmp(root, "/dev/vda2") || target_memory != UINT64_C(2147483648))
        return 1;

    char invalid[] = "nativepipe.memory_target_bytes=not-a-number";
    if (parse_boot_configuration(invalid, &maintenance, root, sizeof(root), &target_memory) >= 0)
        return 1;
    char unaligned[] = "nativepipe.memory_target_bytes=1048577";
    return parse_boot_configuration(unaligned, &maintenance, root, sizeof(root), &target_memory) < 0
               ? 0
               : 1;
}

static int read_boot_configuration(bool *maintenance, char *root, size_t root_capacity,
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
    return parse_boot_configuration(
        command_line, maintenance, root, root_capacity, target_memory);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    setup_runtime();
    bool maintenance = false;
    uint64_t target_memory = 0;
    struct np_plan boot_plan;
    memset(&boot_plan, 0, sizeof(boot_plan));
    if (read_boot_configuration(
            &maintenance, boot_plan.root, sizeof(boot_plan.root), &target_memory) < 0) {
        log_errno("read kernel command line");
        for (;;)
            pause();
    }
    if (target_memory && publish_target_memory(target_memory) < 0)
        log_errno("publish target memory");
    if (!maintenance) {
        if (!valid_root(boot_plan.root)) {
            logmsg("normal boot requires an explicit root= kernel argument");
            for (;;)
                pause();
        }
        logmsg("normal boot; switching directly to the configured root");
        switch_to_root(&boot_plan);
    }
    control_listener = listen_for_host();
    if (control_listener < 0) {
        log_errno("vsock listen");
        for (;;)
            pause();
    }
    logmsg("ready; waiting for host control on vsock port 1024");
    for (;;) {
        control_connection = accept4(control_listener, NULL, NULL, SOCK_CLOEXEC);
        if (control_connection < 0) {
            if (errno != EINTR)
                log_errno("vsock accept");
            continue;
        }
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
        if (control_connection >= 0)
            close(control_connection);
        control_connection = -1;
    }
}
