#define _GNU_SOURCE
#include "install.h"
#include "root.h"
#include "rootfs.h"
#include "np.h"
#include "np_file_rpc.h"
#include "os_release.h"
#include "rosetta.h"

#include <json-c/json.h>
#include <openssl/evp.h>
#include <linux/fs.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

static int error(const char *message) {
    fprintf(stderr, "nativepipe-install: %s: %s\n", message, strerror(errno));
    return -1;
}

static int ensure_network(void) {
    struct stat dns;
    struct ifaddrs *addresses;
    if (stat("/etc/resolv.conf", &dns) == 0 && dns.st_size > 0 && getifaddrs(&addresses) == 0) {
        int ready = 0;
        for (struct ifaddrs *a = addresses; a; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET &&
                (a->ifa_flags & IFF_UP) && !(a->ifa_flags & IFF_LOOPBACK)) ready = 1;
        freeifaddrs(addresses);
        if (ready) return 0;
    }
    DIR *interfaces = opendir("/sys/class/net");
    if (!interfaces) return -1;
    struct dirent *entry;
    int result = -1;
    while ((entry = readdir(interfaces))) {
        char device[512];
        snprintf(device, sizeof(device), "/sys/class/net/%s/device", entry->d_name);
        if (access(device, F_OK) < 0) continue;
        char *up[] = {"/sbin/ip", "link", "set", "dev", entry->d_name, "up", NULL};
        char *lease[] = {"/sbin/udhcpc", "-q", "-n", "-t", "5", "-T", "3", "-i", entry->d_name, NULL};
        if (np_run(up) == 0 && np_run(lease) == 0 &&
            stat("/etc/resolv.conf", &dns) == 0 && dns.st_size > 0) { result = 0; break; }
    }
    closedir(interfaces);
    if (result) { errno = ENETUNREACH; error("no installation network with DNS"); }
    return result;
}

static int run_input(char *const argv[], const char *input) {
    int fds[2];
    if (strlen(input) > 4096 || pipe2(fds, O_CLOEXEC) < 0) return -1;
    if (np_write_full(fds[1], input, strlen(input)) < 0) { close(fds[0]); close(fds[1]); return -1; }
    close(fds[1]);
    pid_t child = fork();
    if (child == 0) {
        if (dup2(fds[0], 0) < 0 || np_child_cloexec() < 0) _exit(126);
        execv(argv[0], argv); _exit(127);
    }
    close(fds[0]);
    if (child < 0) return -1;
    int status;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    return result < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int partition_path(const char *disk, char path[PATH_MAX]) {
    if (!disk || strncmp(disk, "/dev/", 5) || !disk[5] ||
        strspn(disk + 5, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != strlen(disk + 5)) {
        errno = EINVAL; return -1;
    }
    int n = snprintf(path, PATH_MAX, "%s%s1", disk, isdigit((unsigned char)disk[strlen(disk) - 1]) ? "p" : "");
    return n > 0 && n < PATH_MAX ? 0 : -1;
}

static int blank_disk(const char *disk) {
    int fd = open(disk, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISBLK(st.st_mode)) { close(fd); errno = ENOTBLK; return -1; }
    unsigned char data[65536];
    int rc = np_read_full(fd, data, sizeof(data));
    uint64_t size = 0;
    if (rc == 0 && ioctl(fd, BLKGETSIZE64, &size) == 0 && size >= 2 * sizeof(data)) {
        for (size_t i = 0; i < sizeof(data); i++) if (data[i]) { close(fd); return 0; }
        if (lseek(fd, (off_t)(size - sizeof(data)), SEEK_SET) < 0) rc = -1;
        else rc = np_read_full(fd, data, sizeof(data));
    } else rc = -1;
    close(fd);
    if (rc < 0) return -1;
    for (size_t i = 0; i < sizeof(data); i++) if (data[i]) return 0;
    return 1;
}

static int disk_unmounted(const char *disk) {
    char partition[PATH_MAX];
    if (partition_path(disk, partition) < 0) return -1;
    struct stat whole, part;
    if (stat(disk, &whole) < 0 || !S_ISBLK(whole.st_mode)) { errno = ENOTBLK; return -1; }
    int has_part = stat(partition, &part) == 0;
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (!mounts) return -1;
    char *line = NULL;
    size_t capacity = 0;
    int result = 0;
    while (getline(&line, &capacity, mounts) >= 0) {
        unsigned major_number, minor_number;
        if (sscanf(line, "%*u %*u %u:%u", &major_number, &minor_number) != 2) { result = -1; break; }
        dev_t device = makedev(major_number, minor_number);
        if (device == whole.st_rdev || (has_part && device == part.st_rdev)) { result = -1; break; }
    }
    if (ferror(mounts)) result = -1;
    free(line); fclose(mounts);
    if (result) errno = EBUSY;
    return result;
}

static int prepare_disk(const char *disk, const char *root_path) {
    char partition[PATH_MAX];
    if (partition_path(disk, partition) < 0 || disk_unmounted(disk) < 0) return -1;
    int blank = blank_disk(disk);
    if (blank < 0) return -1;
    if (blank) {
        char *table[] = {"/sbin/sfdisk", (char *)disk, NULL};
        if (run_input(table, "label: gpt\nunit: sectors\n\n"
                "start=2048, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=\"nativepipe-root\"\n") != 0) return -1;
        unsigned attempt = 0;
        while (access(partition, F_OK) < 0 && attempt++ < 100) {
            struct timespec delay = {.tv_nsec = 100000000};
            nanosleep(&delay, NULL);
        }
        char *format[] = {"/sbin/mkfs.ext4", "-F", "-L", "nativepipe-root", partition, NULL};
        if (np_run(format) != 0) return -1;
    }
    if (np_mkdir_p(root_path) < 0 || mount(partition, root_path, "ext4", 0, NULL) < 0) return -1;
    return blank;
}

static int repair_disk(const char *disk) {
    char partition[PATH_MAX];
    if (partition_path(disk, partition) < 0 || disk_unmounted(disk) < 0 || blank_disk(disk) != 0) return -1;
    char *extend[] = {"/sbin/sfdisk", "-N", "1", (char *)disk, NULL};
    if (run_input(extend, "size=+\n") != 0) return -1;
    char *check[] = {"/sbin/e2fsck", "-f", "-p", partition, NULL};
    int rc = np_run(check);
    if (rc != 0 && rc != 1) return -1;
    char *resize[] = {"/sbin/resize2fs", partition, NULL};
    return np_run(resize);
}

static json_object *json_field(json_object *object, const char *key, enum json_type type) {
    json_object *value;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, type)) return NULL;
    if (type == json_type_string && (size_t)json_object_get_string_len(value) != strlen(json_object_get_string(value))) return NULL;
    return value;
}

static int source_verified(const char *path, json_object *checksum) {
    json_object *algorithm = json_field(checksum, "algorithm", json_type_string);
    json_object *value = json_field(checksum, "value", json_type_string);
    if (!algorithm || !value) { errno = EINVAL; return -1; }
    const char *name = json_object_get_string(algorithm), *expected = json_object_get_string(value);
    const EVP_MD *method = !strcmp(name, "sha256") ? EVP_sha256() : !strcmp(name, "md5") ? EVP_md5() : NULL;
    if (!method || strlen(expected) != (size_t)EVP_MD_size(method) * 2 ||
        strspn(expected, "0123456789abcdef") != strlen(expected)) { errno = EINVAL; return -1; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    int result = -1;
    if (!context || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > (UINT64_C(8) << 30) || EVP_DigestInit_ex(context, method, NULL) != 1) goto done;
    unsigned char buffer[65536], digest[EVP_MAX_MD_SIZE];
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) break;
        if (EVP_DigestUpdate(context, buffer, (size_t)n) != 1) goto done;
    }
    unsigned length;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1) goto done;
    char actual[EVP_MAX_MD_SIZE * 2 + 1];
    for (unsigned i = 0; i < length; i++) snprintf(actual + i * 2, 3, "%02x", digest[i]);
    if (strcmp(expected, actual)) { errno = EBADMSG; goto done; }
    result = 0;
done:
    EVP_MD_CTX_free(context); close(fd);
    if (result) error("source checksum validation failed before disk preparation");
    return result;
}

static int valid_user(const char *user) {
    return user && strlen(user) > 0 && strlen(user) <= 32 && user[0] >= 'a' && user[0] <= 'z' &&
        strspn(user, "abcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(user) && strcmp(user, "root");
}

static int account(struct np_install *install) {
    char data[sizeof(install->username) + sizeof(install->password) + 3];
    ssize_t n = np_root_read(install->payload, "/account", data, sizeof(data));
    int result = -1;
    if (n < 0 || memchr(data, 0, (size_t)n)) goto done;
    char *password = strchr(data, '\n');
    if (!password) goto done;
    *password++ = 0;
    char *end = strchr(password, '\n');
    if (end) { *end++ = 0; if (*end) goto done; }
    if (!valid_user(data) || !*password || strlen(password) >= sizeof(install->password) || strchr(password, '\r')) goto done;
    strcpy(install->username, data);
    strcpy(install->password, password);
    result = 0;
done:
    explicit_bzero(data, sizeof(data));
    if (result < 0) errno = EINVAL;
    return result;
}

static int stage(int root, const char *id, const char *phase) {
    char state[160];
    int n = snprintf(state, sizeof(state), "NPI1\n%s\n%s\n", id, phase);
    if (n < 0 || n >= (int)sizeof(state) ||
        np_root_write(root, "/.nativepipe-install.new", state, (size_t)n, 0600) < 0 ||
        renameat(root, ".nativepipe-install.new", root, ".nativepipe-install") < 0 || syncfs(root) < 0) return -1;
    fprintf(stderr, "nativepipe-install: %s\n", phase);
    return 0;
}

static int verified_root(struct np_install *install) {
    char release[16384], identity[128];
    if (np_root_read(install->root, "/etc/os-release", release, sizeof(release)) < 0 &&
        np_root_read(install->root, "/usr/lib/os-release", release, sizeof(release)) < 0) return -1;
    if (np_os_release_value(release, "ID", identity, sizeof(identity)) != 1) return -1;
    const char *expected = install->distribution->os_id;
    if (install->translated && install->distribution->packages == NP_PACMAN) expected = "arch";
    if (strcmp(identity, expected)) { errno = EINVAL; return error("rootfs distribution does not match the plan"); }
    int shell = np_file_open(install->root, "/bin/sh", O_RDONLY, 0);
    unsigned char header[20];
    if (shell < 0) return -1;
    int rc = np_read_full(shell, header, sizeof(header));
    close(shell);
#if defined(__aarch64__)
    unsigned native_machine = 183;
#else
    unsigned native_machine = 62;
#endif
    unsigned machine = install->translated ? 62 : native_machine;
    if (rc < 0 || memcmp(header, "\177ELF", 4) || header[4] != 2 || header[5] != 1 ||
        header[18] != machine || header[19] != 0) { errno = ENOEXEC; return error("rootfs architecture mismatch"); }
    return 0;
}

static int install_amd64(struct np_install *native, const char *source,
                         enum np_rootfs_format format, const char *id) {
    struct np_install target = *native;
    target.translated = 1;
    target.root = np_file_open(native->root, "/var/lib/nativepipe/amd64", O_RDONLY | O_DIRECTORY, 0);
    int fresh = target.root < 0 && errno == ENOENT;
    if (fresh) {
        if (np_root_mkdir(native->root, "/var/lib/nativepipe/amd64", 0755) < 0) return -1;
        target.root = np_file_open(native->root, "/var/lib/nativepipe/amd64", O_RDONLY | O_DIRECTORY, 0);
    }
    if (target.root < 0) return -1;
    int result = -1, extracted = 0;
    if (!fresh) {
        char previous[256], expected[48];
        snprintf(expected, sizeof(expected), "NPI1\n%s\n", id);
        if (np_root_read(target.root, "/.nativepipe-install", previous, sizeof(previous)) < 0 ||
            strncmp(previous, expected, strlen(expected))) {
            errno = EEXIST; error("amd64 directory belongs to another installation"); goto done;
        }
        const char *phase = previous + strlen(expected);
        if (!strcmp(phase, "complete\n")) { result = verified_root(&target); goto done; }
        extracted = !strcmp(phase, "extracted\n");
    }
    if (!extracted && (stage(target.root, id, "prepared") < 0 ||
        np_rootfs_extract_at(source, target.root, format, "amd64") < 0 ||
        verified_root(&target) < 0 || stage(target.root, id, "extracted") < 0)) goto done;
    /* Block service startup when the user later upgrades this environment.
     * It shares native services, but retains its own package/account database. */
    if (target.distribution->packages == NP_APT &&
        np_root_write(target.root, "/usr/sbin/policy-rc.d", "#!/bin/sh\nexit 101\n", 19, 0755) < 0) goto done;
    if (np_install_packages(&target) < 0 || np_install_account(&target) < 0 ||
        np_install_amd64_entry(native, &target) < 0 || stage(target.root, id, "complete") < 0) goto done;
    result = 0;
done:
    explicit_bzero(target.password, sizeof(target.password));
    close(target.root);
    return result;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (geteuid() != 0) { errno = EPERM; return error("installation requires root") != 0; }
    const char *action = argc > 1 ? argv[1] : "";
    const char *disk = getenv("NP_TARGET_DISK");
    if (!strcmp(action, "repair")) return repair_disk(disk) == 0 ? 0 : 1;
    if (strcmp(action, "install") && strcmp(action, "directory")) { errno = EINVAL; return error("expected install, directory or repair") != 0; }
    const char *root_path = getenv("NP_TARGET_ROOT"), *source = getenv("NP_SOURCE_PATH");
    if (!root_path || root_path[0] != '/' || !strcmp(root_path, "/") || !source || source[0] != '/' ||
        strlen(source) >= PATH_MAX || strlen(root_path) >= PATH_MAX) { errno = EINVAL; return error("invalid installation paths") != 0; }
    char payload_path[PATH_MAX];
    strcpy(payload_path, source);
    char *last = strrchr(payload_path, '/');
    if (!last || last == payload_path) return 1;
    *last = 0;
    struct np_install install = {.root = -1, .payload = -1};
    install.payload = open(payload_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    char manifest[16384];
    json_object *plan = NULL;
    int mounted = 0, result = 1;
    ssize_t manifest_size = install.payload < 0 ? -1 : np_root_read(install.payload, "/install.json", manifest, sizeof(manifest));
    if (manifest_size < 0 || memchr(manifest, 0, (size_t)manifest_size)) goto done;
    json_tokener *parser = json_tokener_new();
    if (!parser) goto done;
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
    plan = json_tokener_parse_ex(parser, manifest, (int)manifest_size + 1);
    json_tokener_free(parser);
    if (!plan || !json_object_is_type(plan, json_type_object)) { errno = EINVAL; goto done; }
    json_object *distro = json_field(plan, "distribution", json_type_string);
    json_object *format = json_field(plan, "format", json_type_string);
    json_object *identity = json_field(plan, "installationID", json_type_string);
    json_object *software = json_field(plan, "software", json_type_array);
    json_object *checksum = json_field(plan, "sourceChecksum", json_type_object);
    if (!distro || !format || !identity || !software || !checksum || !(install.distribution = np_distribution_find(json_object_get_string(distro)))) {
        errno = EINVAL; goto done;
    }
    const char *id = json_object_get_string(identity);
    if (strlen(id) != 32 || strspn(id, "0123456789abcdef") != 32) { errno = EINVAL; goto done; }
    const char *format_name = json_object_get_string(format);
    enum np_rootfs_format image;
    if (!strcmp(format_name, "tar")) image = NP_ROOTFS_TAR;
    else if (!strcmp(format_name, "oci")) image = NP_ROOTFS_OCI;
    else { errno = EINVAL; goto done; }
    if (json_object_array_length(software) > 8) { errno = EINVAL; goto done; }
    for (size_t i = 0; i < json_object_array_length(software); i++) {
        json_object *option = json_object_array_get_idx(software, i);
        if (!json_object_is_type(option, json_type_string) ||
            (size_t)json_object_get_string_len(option) != strlen(json_object_get_string(option))) { errno = EINVAL; goto done; }
        const char *name = json_object_get_string(option);
        if (!strcmp(name, "x86_64")) install.rosetta = 1;
        else if (!strcmp(name, "developer-tools")) install.developer = 1;
        else if (!strcmp(name, "wine") && !strcmp(install.distribution->id, "ubuntu")) {
            install.wine = 1; install.rosetta = 1;
        } else if (!strcmp(name, "steam") && !strcmp(install.distribution->id, "ubuntu")) install.steam = 1;
        else if (!strcmp(name, "amd64-rootfs") && install.distribution->packages != NP_APK) {
            install.amd64 = 1; install.rosetta = 1;
        }
        else { errno = ENOTSUP; error("unsupported software option"); goto done; }
    }
    char amd64_source[PATH_MAX] = "";
    enum np_rootfs_format amd64_format = NP_ROOTFS_TAR;
    if (install.amd64) {
        json_object *format = json_field(plan, "amd64Format", json_type_string);
        json_object *checksum = json_field(plan, "amd64Checksum", json_type_object);
        if (!format || !checksum ||
            snprintf(amd64_source, sizeof(amd64_source), "%s/source-amd64", payload_path) >= (int)sizeof(amd64_source)) {
            errno = EINVAL; goto done;
        }
        const char *name = json_object_get_string(format);
        if (!strcmp(name, "oci")) amd64_format = NP_ROOTFS_OCI;
        else if (!strcmp(name, "arch-bootstrap")) amd64_format = NP_ROOTFS_ARCH_BOOTSTRAP;
        else if (strcmp(name, "tar")) { errno = EINVAL; goto done; }
        if (source_verified(amd64_source, checksum) < 0) goto done;
        const char *files[] = {"/amd64-graphics/nativepipe-align-blob-x86_64-gnu.so",
            "/amd64-graphics/nativepipe-vulkan-layer-x86_64-gnu.so",
            "/amd64-graphics/VkLayer_NATIVEPIPE_blob_alignment.json", "/amd64-graphics/nativepipe.sh"};
        for (unsigned i = 0; i < 4; i++) {
            int fd = np_file_open(install.payload, files[i], O_RDONLY, 0);
            if (fd < 0) goto done;
            close(fd);
        }
    }
    if (account(&install) < 0 || source_verified(source, checksum) < 0 ||
        (install.rosetta && np_rosetta_prepare() < 0) || ensure_network() < 0) goto done;
    int guest = np_file_open(install.payload, "/agent/nativepipe-guestd", O_RDONLY, 0);
    if (guest < 0) goto done;
    close(guest);
    const char *service = install.distribution->init == NP_OPENRC
        ? "/agent/openrc/nativepipe-guestd" : "/agent/systemd/nativepipe-guestd.service";
    guest = np_file_open(install.payload, service, O_RDONLY, 0);
    if (guest < 0) goto done;
    close(guest);
    int fresh;
    if (!strcmp(action, "directory")) {
        fresh = mkdir(root_path, 0700) == 0;
        if (!fresh && errno != EEXIST) goto done;
    } else {
        fresh = prepare_disk(disk, root_path);
        if (fresh < 0) goto done;
        mounted = 1;
    }
    install.root = open(root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (install.root < 0 || fchmod(install.root, 0755) < 0) goto done;
    char previous[256], expected[48];
    int complete = 0, extracted = 0;
    snprintf(expected, sizeof(expected), "NPI1\n%s\n", id);
    if (!fresh) {
        if (np_root_read(install.root, "/.nativepipe-install", previous, sizeof(previous)) < 0 ||
            strncmp(previous, expected, strlen(expected))) {
            errno = EEXIST; error("target belongs to another installation; refusing to format or overwrite"); goto done;
        }
        const char *phase = previous + strlen(expected);
        complete = !strcmp(phase, "complete\n");
        extracted = complete || !strcmp(phase, "extracted\n");
    }
    if (complete) { result = verified_root(&install) == 0 ? 0 : 1; goto done; }
    if (!extracted) {
        if (stage(install.root, id, "prepared") < 0 || np_rootfs_extract(source, root_path, image,
#if defined(__aarch64__)
                "arm64"
#else
                "amd64"
#endif
                ) < 0 || verified_root(&install) < 0 || stage(install.root, id, "extracted") < 0) goto done;
    }
    if (np_install_packages(&install) < 0 ||
        np_install_account(&install) < 0 ||
        (install.amd64 && install_amd64(&install, amd64_source, amd64_format, id) < 0) ||
        np_install_software(&install) < 0 || np_install_guest(&install) < 0 ||
        np_install_configure(&install) < 0 || stage(install.root, id, "complete") < 0) goto done;
    result = 0;
done:
    if (result) fputs("nativepipe-install: installation failed; see the preceding operation error\n", stderr);
    explicit_bzero(install.password, sizeof(install.password));
    if (plan) json_object_put(plan);
    if (install.root >= 0) close(install.root);
    if (install.payload >= 0) close(install.payload);
    if (mounted && result && umount2(root_path, 0) < 0) error("cannot unmount failed installation");
    return result;
}
