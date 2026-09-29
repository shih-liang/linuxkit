#define _GNU_SOURCE
#include "rootfs.h"

#include <archive.h>
#include <archive_entry.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_JSON (1024u * 1024u)
#define MAX_ENTRIES 1000000u
#define MAX_EXPANDED (64ULL * 1024 * 1024 * 1024)

static int failure(const char *detail) {
    fprintf(stderr, "nativepipe-install: rootfs: %s\n", detail);
    errno = EINVAL;
    return -1;
}

static const char *relative_path(const char *p) {
    if (!p || *p == '/') return NULL;
    while (!strncmp(p, "./", 2)) p += 2;
    if (!strcmp(p, ".")) return "";
    for (const char *c = p; *c; ) {
        const char *end = strchr(c, '/');
        size_t n = end ? (size_t)(end - c) : strlen(c);
        if (n == 2 && !memcmp(c, "..", 2)) return NULL;
        if (!end) break;
        c = end + 1;
    }
    return p;
}

/* Deletion is relative to already opened directory descriptors. A whiteout
 * cannot traverse a symlink created by an earlier layer. */
static int remove_entry(int parent, const char *name) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlinkat(parent, name, 0);
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return -1; }
    int rc = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (remove_entry(fd, entry->d_name) < 0) { rc = -1; break; }
    }
    closedir(dir);
    return rc < 0 ? rc : unlinkat(parent, name, AT_REMOVEDIR);
}

static int whiteout(const char *path) {
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof(copy)) return -1;
    strcpy(copy, path);
    int parent = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return -1;
    char *name = copy, *slash;
    while ((slash = strchr(name, '/'))) {
        *slash = 0;
        if (*name && strcmp(name, ".")) {
            int next = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            close(parent);
            if (next < 0) return errno == ENOENT ? 0 : -1;
            parent = next;
        }
        name = slash + 1;
    }
    int rc = 0;
    if (!strcmp(name, ".wh..wh..opq")) {
        int fd = dup(parent);
        DIR *dir = fd < 0 ? NULL : fdopendir(fd);
        if (!dir) { if (fd >= 0) close(fd); close(parent); return -1; }
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (remove_entry(parent, entry->d_name) < 0) { rc = -1; break; }
        }
        closedir(dir);
    } else if (!strncmp(name, ".wh.", 4) && name[4] &&
               strcmp(name + 4, ".") && strcmp(name + 4, "..")) {
        rc = remove_entry(parent, name + 4);
    } else rc = -1;
    close(parent);
    return rc;
}

static struct archive *reader(int fd) {
    struct archive *a = archive_read_new();
    if (!a) return NULL;
    archive_read_support_filter_none(a);
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_xz(a);
    archive_read_support_filter_zstd(a);
    archive_read_support_format_tar(a);
    if (lseek(fd, 0, SEEK_SET) < 0 || archive_read_open_fd(a, fd, 65536) != ARCHIVE_OK) {
        failure(archive_error_string(a) ? archive_error_string(a) : "cannot open archive");
        archive_read_free(a);
        return NULL;
    }
    return a;
}

static int extract_tar(int fd, const char *prefix, int layer, int envelope) {
    /* Whiteouts precede all additions, including when the tar orders them last. */
    for (int pass = layer ? 0 : 1; pass <= 1; pass++) {
        struct archive *a = reader(fd), *disk = archive_write_disk_new();
        if (!a || !disk) { if (a) archive_read_free(a); if (disk) archive_write_free(disk); return -1; }
        int options = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
            ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_XATTR | ARCHIVE_EXTRACT_FFLAGS |
            ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
            ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS;
        if (geteuid() == 0) options |= ARCHIVE_EXTRACT_OWNER;
        archive_write_disk_set_options(disk, options);
        struct archive_entry *entry;
        unsigned count = 0;
        uint64_t bytes = 0;
        int rc = -1, status;
        while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
            const char *raw = relative_path(archive_entry_pathname(entry));
            if (!raw || strlen(raw) >= PATH_MAX || ++count > MAX_ENTRIES) {
                failure("unsafe archive path or too many entries"); goto done;
            }
            const char *path = raw;
            if (prefix && *prefix) {
                size_t n = strlen(prefix);
                if (strncmp(raw, prefix, n) || (raw[n] && raw[n] != '/')) {
                    failure("unexpected archive root"); goto done;
                }
                path = raw + n;
                if (*path == '/') path++;
            }
            if (!*path) { archive_read_data_skip(a); continue; }
            char path_copy[PATH_MAX];
            strcpy(path_copy, path);
            archive_entry_set_pathname(entry, path_copy);
            const char *base = strrchr(path_copy, '/');
            base = base ? base + 1 : path_copy;
            if (layer && !strncmp(base, ".wh.", 4)) {
                if (pass == 0 && whiteout(path_copy) < 0) {
                    failure("unsafe layer whiteout"); goto done;
                }
                archive_read_data_skip(a); continue;
            }
            if (pass == 0) { archive_read_data_skip(a); continue; }
            mode_t type = archive_entry_filetype(entry);
            const char *hard = archive_entry_hardlink(entry);
            if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK && !(type == 0 && hard)) {
                /* /dev is supplied by the kernel, never by an archive. */
                failure("unsupported special file in rootfs"); goto done;
            }
            if (envelope && (type == AE_IFLNK || hard)) {
                failure("OCI envelope contains a link"); goto done;
            }
            if (hard) {
                hard = relative_path(hard);
                if (!hard) { failure("unsafe hardlink"); goto done; }
                if (prefix && *prefix) {
                    size_t n = strlen(prefix);
                    if (strncmp(hard, prefix, n) || hard[n] != '/') {
                        failure("hardlink escapes archive root"); goto done;
                    }
                    hard += n + 1;
                }
                archive_entry_set_hardlink(entry, hard);
            }
            int64_t size = archive_entry_size(entry);
            if (size < 0 || (uint64_t)size > MAX_EXPANDED - bytes) {
                failure("expanded rootfs is too large"); goto done;
            }
            bytes += (uint64_t)size;
            if (archive_write_header(disk, entry) != ARCHIVE_OK) {
                failure(archive_error_string(disk)); goto done;
            }
            const void *block;
            size_t length;
            la_int64_t offset;
            int read_status;
            while ((read_status = archive_read_data_block(a, &block, &length, &offset)) == ARCHIVE_OK)
                if (archive_write_data_block(disk, block, length, offset) != ARCHIVE_OK) {
                    failure(archive_error_string(disk)); goto done;
                }
            if (read_status != ARCHIVE_EOF || archive_write_finish_entry(disk) != ARCHIVE_OK) {
                failure("truncated or invalid archive entry"); goto done;
            }
        }
        if (status != ARCHIVE_EOF) { failure("truncated or invalid archive"); goto done; }
        rc = 0;
done:
        if (archive_write_close(disk) != ARCHIVE_OK) rc = -1;
        archive_write_free(disk);
        archive_read_free(a);
        if (rc < 0) return -1;
    }
    return 0;
}

static json_object *read_json(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 2 || st.st_size > MAX_JSON)
        return NULL;
    char *data = malloc((size_t)st.st_size + 1);
    if (!data) return NULL;
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t n = pread(fd, data + used, (size_t)st.st_size - used, (off_t)used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); return NULL; }
        used += (size_t)n;
    }
    data[used] = 0;
    struct json_tokener *tok = json_tokener_new_ex(16);
    if (!tok) { free(data); return NULL; }
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    json_object *object = json_tokener_parse_ex(tok, data, (int)used + 1);
    if (json_tokener_get_error(tok) != json_tokener_success ||
        !json_object_is_type(object, json_type_object)) {
        if (object) json_object_put(object);
        object = NULL;
    }
    json_tokener_free(tok);
    free(data);
    return object;
}

static json_object *field(json_object *object, const char *name, enum json_type type) {
    json_object *value = NULL;
    return json_object_object_get_ex(object, name, &value) && json_object_is_type(value, type)
        ? value : NULL;
}

static int blob(int directory, json_object *descriptor) {
    json_object *digest = field(descriptor, "digest", json_type_string);
    json_object *size = field(descriptor, "size", json_type_int);
    if (!digest || !size) return failure("missing OCI digest or size");
    const char *hash = json_object_get_string(digest);
    if (strlen(hash) != 71 || strncmp(hash, "sha256:", 7) ||
        strspn(hash + 7, "0123456789abcdef") != 64)
        return failure("unsupported OCI digest");
    char path[80];
    snprintf(path, sizeof(path), "blobs/sha256/%s", hash + 7);
    int fd = openat(directory, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size != json_object_get_int64(size)) {
        close(fd); return failure("OCI blob size mismatch");
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned char output[EVP_MAX_MD_SIZE], block[65536];
    unsigned length = 0;
    int ok = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    ssize_t n = -1;
    while (ok && (n = read(fd, block, sizeof(block))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; ok = 0; break; }
        ok = EVP_DigestUpdate(context, block, (size_t)n) == 1;
    }
    if (ok) ok = EVP_DigestFinal_ex(context, output, &length) == 1 && length == 32;
    EVP_MD_CTX_free(context);
    char actual[65];
    if (ok) for (unsigned i = 0; i < 32; i++) snprintf(actual + 2 * i, 3, "%02x", output[i]);
    if (!ok || strcmp(actual, hash + 7)) { close(fd); return failure("OCI blob digest mismatch"); }
    if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }
    return fd;
}

static int apply_oci(int directory, json_object *manifest, const char *architecture, unsigned depth) {
    if (depth > 4) return failure("OCI index nesting is too deep");
    json_object *schema = field(manifest, "schemaVersion", json_type_int);
    if (!schema || json_object_get_int(schema) != 2) return failure("unsupported OCI schema");
    json_object *manifests = field(manifest, "manifests", json_type_array);
    if (manifests) {
        json_object *selected = NULL;
        size_t count = json_object_array_length(manifests);
        if (count > 64) return failure("too many OCI manifests");
        for (size_t i = 0; i < count; i++) {
            json_object *d = json_object_array_get_idx(manifests, i);
            json_object *platform = field(d, "platform", json_type_object);
            json_object *arch = platform ? field(platform, "architecture", json_type_string) : NULL;
            json_object *os = platform ? field(platform, "os", json_type_string) : NULL;
            if (platform && (!arch || !os || strcmp(json_object_get_string(arch), architecture) ||
                             strcmp(json_object_get_string(os), "linux"))) continue;
            if (!platform && count != 1) continue;
            if (selected) return failure("ambiguous OCI platform");
            selected = d;
        }
        if (!selected) return failure("OCI image does not contain selected architecture");
        int fd = blob(directory, selected);
        if (fd < 0) return -1;
        json_object *child = read_json(fd);
        close(fd);
        if (!child) return failure("invalid OCI manifest");
        int rc = apply_oci(directory, child, architecture, depth + 1);
        json_object_put(child);
        return rc;
    }
    json_object *config = field(manifest, "config", json_type_object);
    json_object *layers = field(manifest, "layers", json_type_array);
    if (!config || !layers || json_object_array_length(layers) == 0 ||
        json_object_array_length(layers) > 128) return failure("invalid OCI image");
    int fd = blob(directory, config);
    if (fd < 0) return -1;
    json_object *settings = read_json(fd);
    close(fd);
    json_object *arch = settings ? field(settings, "architecture", json_type_string) : NULL;
    json_object *os = settings ? field(settings, "os", json_type_string) : NULL;
    int matches = arch && os && !strcmp(json_object_get_string(arch), architecture) &&
        !strcmp(json_object_get_string(os), "linux");
    if (settings) json_object_put(settings);
    if (!matches) return failure("OCI configuration architecture mismatch");
    for (size_t i = 0; i < json_object_array_length(layers); i++) {
        json_object *layer = json_object_array_get_idx(layers, i);
        json_object *type = field(layer, "mediaType", json_type_string);
        const char *name = type ? json_object_get_string(type) : "";
        if (strcmp(name, "application/vnd.oci.image.layer.v1.tar") &&
            strcmp(name, "application/vnd.oci.image.layer.v1.tar+gzip") &&
            strcmp(name, "application/vnd.oci.image.layer.v1.tar+zstd"))
            return failure("unsupported OCI layer type");
        fd = blob(directory, layer);
        if (fd < 0) return -1;
        int rc = extract_tar(fd, NULL, 1, 0);
        close(fd);
        if (rc < 0) return -1;
    }
    return 0;
}

int np_rootfs_extract_at(const char *source, int target,
                        enum np_rootfs_format format, const char *architecture) {
    int cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int root = fcntl(target, F_DUPFD_CLOEXEC, 3);
    int input = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int rc = -1;
    char work[] = "/tmp/nativepipe-oci.XXXXXX";
    int have_work = 0, directory = -1;
    if (cwd < 0 || root < 0 || input < 0) goto done;
    if (format != NP_ROOTFS_OCI) {
        if (fchdir(root) < 0) goto done;
        rc = extract_tar(input, format == NP_ROOTFS_ARCH_BOOTSTRAP ? "root.x86_64" : NULL, 0, 0);
    } else {
        if (!mkdtemp(work)) goto done;
        have_work = 1;
        if (chdir(work) < 0 || extract_tar(input, NULL, 0, 1) < 0) goto done;
        directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) goto done;
        int index = openat(directory, "index.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (index < 0) goto done;
        json_object *manifest = read_json(index);
        close(index);
        if (!manifest) { failure("invalid OCI index"); goto done; }
        if (fchdir(root) == 0) rc = apply_oci(directory, manifest, architecture, 0);
        json_object_put(manifest);
    }
done:
    if (cwd >= 0) { if (fchdir(cwd) < 0) rc = -1; close(cwd); }
    if (directory >= 0) close(directory);
    if (input >= 0) close(input);
    if (root >= 0) close(root);
    if (have_work) {
        int parent = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) { if (remove_entry(parent, strrchr(work, '/') + 1) < 0) rc = -1; close(parent); }
        else rc = -1;
    }
    return rc;
}

int np_rootfs_extract(const char *source, const char *target,
                     enum np_rootfs_format format, const char *architecture) {
    int root = open(target, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return -1;
    int result = np_rootfs_extract_at(source, root, format, architecture);
    close(root);
    return result;
}
