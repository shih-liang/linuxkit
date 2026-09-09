#define _GNU_SOURCE
#include "np.h"
#include "np_file_rpc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/close_range.h>
#include <sys/syscall.h>
#endif

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

struct sockaddr_vm {
    sa_family_t svm_family;
    unsigned short svm_reserved1;
    unsigned int svm_port;
    unsigned int svm_cid;
    unsigned char svm_zero[sizeof(struct sockaddr) - sizeof(sa_family_t) -
                           sizeof(unsigned short) - sizeof(unsigned int) -
                           sizeof(unsigned int)];
};

static int socket_stream_cloexec(int domain) {
    int type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif
    int fd = socket(domain, type, 0);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int np_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) {
            errno = EIO;
            return -1;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

int np_write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!w) { errno = EIO; return -1; }
        sent += (size_t)w;
    }
    return 0;
}

int np_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int np_venus_icd_available(void) {
    static const char *const directories[] = {
        "/usr/share/vulkan/icd.d",
        "/etc/vulkan/icd.d",
    };
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        DIR *dir = opendir(directories[i]);
        if (!dir)
            continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            size_t length = strlen(name);
            if (strncmp(name, "virtio_icd", 10) == 0 && length >= 5 &&
                strcmp(name + length - 5, ".json") == 0) {
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }
    return 0;
}

int np_mkdir_p(const char *path) {
    char tmp[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int np_write_file(const char *path, const void *data, size_t n, int mode) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (np_mkdir_p(dir) < 0)
            return -1;
    }
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    int rc = np_write_full(fd, data, n);
    if (rc == 0) rc = fchmod(fd, (mode_t)mode);
    int saved = errno;
    if (close(fd) < 0 && rc == 0) return -1;
    errno = saved;
    return rc;
}

int np_write_version(const char *ver) {
    if (!ver || !ver[0])
        return 0;
    char buf[NP_MAX_VERSION + 2];
    snprintf(buf, sizeof(buf), "%s\n", ver);
    if (np_mkdir_p("/usr/libexec/nativepipe") < 0)
        return -1;
    return np_write_file(NP_INSTALLED_VERSION, buf, strlen(buf), 0644);
}

int np_read_version(char *out, size_t cap) {
    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    FILE *f = fopen(NP_INSTALLED_VERSION, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return 0;
}

int np_copy_file(const char *src, const char *dst, int mode) {
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", dst);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        np_mkdir_p(dir);
    }
    int out = open(dst, O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (out < 0) {
        close(in);
        return -1;
    }
    uint8_t buf[65536];
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(in);
            close(out);
            return -1;
        }
        if (r == 0)
            break;
        if (np_write_full(out, buf, (size_t)r) < 0) {
            close(in);
            close(out);
            return -1;
        }
    }
    close(in);
    close(out);
    chmod(dst, (mode_t)mode);
    return 0;
}

int np_child_cloexec(void) {
#ifdef __linux__
    /* In the forked child only. This covers even descriptors another agent
     * thread opened just before fork, without a million-FD scanning loop. */
    return (int)syscall(SYS_close_range, 3u, ~0u, CLOSE_RANGE_CLOEXEC);
#else
    /* Common transport unit tests also build on macOS; guest programs do not. */
    return 0;
#endif
}

int np_run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (np_child_cloexec() < 0) _exit(126);
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

int np_vsock_connect_host(uint32_t port, int retries) {
    if (retries < 1)
        retries = 1;
    int fd = -1;
    for (int attempt = 0; attempt < retries; attempt++) {
        if (fd >= 0)
            close(fd);
        fd = socket_stream_cloexec(AF_VSOCK);
        if (fd < 0)
            return -1;
        struct sockaddr_vm addr;
        memset(&addr, 0, sizeof(addr));
        addr.svm_family = AF_VSOCK;
        addr.svm_cid = NP_CID_HOST;
        addr.svm_port = port;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        if (attempt + 1 < retries)
            sleep(1);
    }
    if (fd >= 0)
        close(fd);
    return -1;
}

int np_vsock_peer_is_host(int fd) {
    struct sockaddr_vm peer;
    socklen_t size = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    return getpeername(fd, (struct sockaddr *)&peer, &size) == 0 &&
           size >= offsetof(struct sockaddr_vm, svm_zero) &&
           peer.svm_family == AF_VSOCK && peer.svm_cid == NP_CID_HOST;
}

int np_vsock_listen(uint32_t port, int backlog) {
    int fd = socket_stream_cloexec(AF_VSOCK);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_vm addr;
    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = NP_CID_ANY;
    addr.svm_port = port;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog > 0 ? backlog : 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int np_agent_send_request(int fd, const char *name, const char *ver) {
    if (!name)
        name = "";
    if (!ver)
        ver = "";
    size_t name_len = strlen(name);
    size_t ver_len = strlen(ver);
    if (name_len > NP_MAX_NAME || ver_len > NP_MAX_VERSION) {
        errno = EINVAL;
        return -1;
    }
    uint8_t hdr[8];
    memcpy(hdr, NP_AGENT_MAGIC, 4);
    hdr[4] = NP_AGENT_WIRE_VERSION;
    hdr[5] = 1; /* Request the shared NPFR chunked payload. */
    hdr[6] = (uint8_t)(name_len & 0xff);
    hdr[7] = (uint8_t)((name_len >> 8) & 0xff);
    uint8_t ver_len_buf[2] = {(uint8_t)(ver_len & 0xff), (uint8_t)((ver_len >> 8) & 0xff)};
    if (np_write_full(fd, hdr, sizeof(hdr)) < 0)
        return -1;
    if (name_len && np_write_full(fd, name, name_len) < 0)
        return -1;
    if (np_write_full(fd, ver_len_buf, 2) < 0)
        return -1;
    if (ver_len && np_write_full(fd, ver, ver_len) < 0)
        return -1;
    return 0;
}

int np_agent_recv_hdr(int fd, np_agent_hdr *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    uint8_t rhdr[8];
    if (np_read_full(fd, rhdr, sizeof(rhdr)) < 0)
        return -1;
    if (memcmp(rhdr, NP_AGENT_MAGIC, 4) != 0 || rhdr[4] != NP_AGENT_WIRE_VERSION) {
        errno = EPROTO;
        return -1;
    }
    hdr->status = rhdr[5];
    uint16_t ver_len = (uint16_t)rhdr[6] | ((uint16_t)rhdr[7] << 8);
    if (ver_len >= NP_MAX_VERSION) {
        errno = EMSGSIZE;
        return -1;
    }
    if (ver_len && np_read_full(fd, hdr->version, ver_len) < 0)
        return -1;
    hdr->version[ver_len] = '\0';
    hdr->payload_len = 0;
    if (hdr->status == NP_STATUS_FILE || hdr->status == NP_STATUS_FORCE) {
        uint8_t lenbuf[8];
        if (np_read_full(fd, lenbuf, 8) < 0)
            return -1;
        uint64_t n = 0;
        for (int i = 0; i < 8; i++)
            n |= ((uint64_t)lenbuf[i]) << (8 * i);
        hdr->payload_len = n;
        if (n == 0 || n > NP_MAX_AGENT_PAYLOAD) {
            errno = EMSGSIZE;
            return -1;
        }
    }
    return 0;
}

static int mkdir_parent(const char *path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return 0;
    *slash = '\0';
    return np_mkdir_p(dir);
}

int np_agent_recv_payload_file(int fd, uint64_t len, const char *path, int mode) {
    /*
     * Receive beside the destination, then atomically rename. A /tmp staging
     * file crosses filesystems on common cloud images; the old EXDEV fallback
     * used one shared "path.new" name, so bootcmd and runcmd recovery attempts
     * could truncate each other. A unique adjacent file is both race-safe and
     * guaranteed to be on the rename target's filesystem.
     */
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }
    if (mkdir_parent(path) < 0)
        return -1;
    char tmp[640];
    if (snprintf(tmp, sizeof(tmp), "%s.new.XXXXXX", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int out = mkstemp(tmp);
    if (out < 0)
        return -1;
    fcntl(out, F_SETFD, FD_CLOEXEC);
    uint64_t received = 0;
    if (np_file_receive_stream(fd, out, len, &received) < 0 || received != len) {
        int error = errno ? errno : EPROTO;
        close(out); unlink(tmp); errno = error; return -1;
    }
    int finalize_rc = 0;
    if (fchmod(out, (mode_t)mode) < 0 || fsync(out) < 0)
        finalize_rc = -1;
    int saved = errno;
    if (close(out) < 0 && finalize_rc == 0) {
        finalize_rc = -1;
        saved = errno;
    }
    if (finalize_rc < 0) {
        unlink(tmp);
        errno = saved;
        return -1;
    }
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int np_agent_recv_payload_mem(int fd, uint64_t len, uint8_t **out, size_t *out_len) {
    if (!len || len > NP_MAX_AGENT_PAYLOAD || len > SIZE_MAX) { errno = EMSGSIZE; return -1; }
    uint8_t *buf = malloc((size_t)len);
    if (!buf)
        return -1;
    uint64_t total = 0;
    struct np_file_frame frame;
    for (;;) {
        if (np_file_receive(fd, &frame) < 0) { free(buf); return -1; }
        if (frame.status) { free(buf); errno = (int)frame.status; return -1; }
        if (frame.type == NP_FILE_END && frame.length == 8 && total == len &&
            np_file_u64(frame.data) == total) break;
        if (frame.type != NP_FILE_DATA || !frame.length || frame.length > len - total) {
            free(buf); errno = EPROTO; return -1;
        }
        memcpy(buf + total, frame.data, frame.length); total += frame.length;
    }
    *out = buf;
    *out_len = (size_t)len;
    return 0;
}

int np_agent_discard_payload(int fd, uint64_t len) {
    int out = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (out < 0) return -1;
    uint64_t received = 0;
    int rc = np_file_receive_stream(fd, out, len, &received);
    int error = errno; close(out); errno = error;
    if (rc == 0 && received != len) { errno = EPROTO; return -1; }
    return rc;
}

static int pull_common(const char *name, const char *ver, int *out_fd, np_agent_hdr *hdr,
                       int retries) {
    int fd = np_vsock_connect_host(NP_PORT_AGENT, retries);
    if (fd < 0)
        return -1;
    if (np_agent_send_request(fd, name, ver) < 0) {
        close(fd);
        return -1;
    }
    if (np_agent_recv_hdr(fd, hdr) < 0) {
        close(fd);
        return -1;
    }
    *out_fd = fd;
    return 0;
}

int np_agent_pull_file_mode_n(const char *name, const char *ver,
                              const char *dest_path, int mode,
                              char *host_ver, size_t host_ver_cap, int retries) {
    if (mode < 0 || (mode & ~0777) != 0) {
        errno = EINVAL;
        return -1;
    }
    int fd;
    np_agent_hdr hdr;
    if (pull_common(name, ver, &fd, &hdr, retries) < 0)
        return -1;
    if (host_ver && host_ver_cap) {
        snprintf(host_ver, host_ver_cap, "%s", hdr.version);
    }
    int rc;
    if (hdr.status == NP_STATUS_UPTODATE) {
        rc = 1;
    } else if (hdr.status == NP_STATUS_NOTFOUND) {
        rc = 2;
    } else if (hdr.status == NP_STATUS_FILE || hdr.status == NP_STATUS_FORCE) {
        if (np_agent_recv_payload_file(fd, hdr.payload_len, dest_path, mode) < 0)
            rc = -1;
        else
            rc = 0;
    } else {
        rc = -1;
    }
    close(fd);
    return rc;
}

int np_agent_pull_file_n(const char *name, const char *ver, const char *dest_path,
                         char *host_ver, size_t host_ver_cap, int retries) {
    return np_agent_pull_file_mode_n(name, ver, dest_path, 0755,
                                     host_ver, host_ver_cap, retries);
}

int np_agent_pull_file(const char *name, const char *ver, const char *dest_path,
                       char *host_ver, size_t host_ver_cap) {
    return np_agent_pull_file_n(name, ver, dest_path, host_ver, host_ver_cap, 300);
}

int np_agent_pull_mem_n(const char *name, const char *ver, uint8_t **mem, size_t *len,
                        char *host_ver, size_t host_ver_cap, int retries) {
    int fd;
    np_agent_hdr hdr;
    if (pull_common(name, ver, &fd, &hdr, retries) < 0)
        return -1;
    if (host_ver && host_ver_cap)
        snprintf(host_ver, host_ver_cap, "%s", hdr.version);
    int rc;
    if (hdr.status == NP_STATUS_UPTODATE) {
        rc = 1;
    } else if (hdr.status == NP_STATUS_NOTFOUND) {
        rc = 2;
    } else if (hdr.status == NP_STATUS_FILE || hdr.status == NP_STATUS_FORCE) {
        if (np_agent_recv_payload_mem(fd, hdr.payload_len, mem, len) < 0)
            rc = -1;
        else
            rc = 0;
    } else {
        rc = -1;
    }
    close(fd);
    return rc;
}

int np_agent_pull_mem(const char *name, const char *ver, uint8_t **mem, size_t *len,
                      char *host_ver, size_t host_ver_cap) {
    return np_agent_pull_mem_n(name, ver, mem, len, host_ver, host_ver_cap, 300);
}

int np_npip_send(int fd, const void *json, size_t json_len) {
    if (json_len > NP_MAX_NPIP_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    uint8_t hdr[8];
    memcpy(hdr, NP_NPIP_MAGIC, 4);
    hdr[4] = NP_WIRE_VERSION;
    hdr[5] = 0;
    hdr[6] = 0;
    hdr[7] = 0;
    uint32_t n = (uint32_t)json_len;
    /* payload length is u32 LE at offset 8 in the 12-byte header:
       "NPIP" | version(1) | 3 reserved | length(u32 LE) */
    uint8_t frame[12];
    memcpy(frame, NP_NPIP_MAGIC, 4);
    frame[4] = NP_WIRE_VERSION;
    frame[5] = 0;
    frame[6] = 0;
    frame[7] = 0;
    frame[8] = (uint8_t)(n & 0xff);
    frame[9] = (uint8_t)((n >> 8) & 0xff);
    frame[10] = (uint8_t)((n >> 16) & 0xff);
    frame[11] = (uint8_t)((n >> 24) & 0xff);
    (void)hdr;
    if (np_write_full(fd, frame, sizeof(frame)) < 0)
        return -1;
    return np_write_full(fd, json, json_len);
}

ssize_t np_npip_recv(int fd, uint8_t **out) {
    uint8_t hdr[12];
    if (np_read_full(fd, hdr, sizeof(hdr)) < 0)
        return -1;
    if (memcmp(hdr, NP_NPIP_MAGIC, 4) != 0 || hdr[4] != NP_WIRE_VERSION) {
        errno = EPROTO;
        return -1;
    }
    uint32_t n = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                 ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    if (n > NP_MAX_NPIP_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    uint8_t *buf = malloc(n + 1);
    if (!buf)
        return -1;
    if (n && np_read_full(fd, buf, n) < 0) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';
    *out = buf;
    return (ssize_t)n;
}
