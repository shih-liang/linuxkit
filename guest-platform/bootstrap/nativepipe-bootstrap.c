/*
 * nativepipe-bootstrap — pull guestd over vsock, run --provision, exit.
 * I/O and NPAG live in ../common/np.c
 *
 * Always pulls guestd when cidata runs. Do not skip based on an existing
 * path: a prior failed transfer can leave a useless file, and the host only
 * attaches cidata when recovery is needed.
 *
 * Rewrites the guest cloud-init instance-id before doing any work so:
 *   - each machine gets a unique id (the shared cidata seed is not unique)
 *   - the next boot with cidata attached is a new instance and retries
 */
#include "np.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BOOTSTRAP_LOCK "/run/nativepipe-bootstrap.lock"
#define BOOTSTRAP_DONE "/run/nativepipe-bootstrap.done"

static void die(const char *msg) {
    fprintf(stderr, "[bootstrap] %s\n", msg);
    exit(1);
}

static void strip_nl(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

/* bootcmd starts early while runcmd is a fallback. They may overlap, but only
 * one installer may receive or provision files during a given boot. */
static int acquire_install_lock(void) {
    int fd = open(BOOTSTRAP_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    while (flock(fd, LOCK_EX) < 0) {
        if (errno == EINTR)
            continue;
        close(fd);
        return -1;
    }
    if (np_path_exists(BOOTSTRAP_DONE)) {
        fprintf(stderr, "[bootstrap] guestd already installed this boot\n");
        close(fd);
        return -2;
    }
    return fd;
}

static int valid_guestd_elf(void) {
    struct stat st;
    if (stat(NP_INSTALLED_BIN, &st) < 0 || st.st_size < 4096 || !(st.st_mode & 0111))
        return 0;
    int fd = open(NP_INSTALLED_BIN, O_RDONLY);
    if (fd < 0)
        return 0;
    unsigned char magic[4];
    ssize_t n = read(fd, magic, sizeof(magic));
    close(fd);
    return n == (ssize_t)sizeof(magic) && magic[0] == 0x7f && magic[1] == 'E' &&
           magic[2] == 'L' && magic[3] == 'F';
}

static void bump_instance_id(void) {
    char uuid[64];
    uuid[0] = '\0';
    FILE *f = fopen("/proc/sys/kernel/random/uuid", "r");
    if (f) {
        if (fgets(uuid, sizeof(uuid), f))
            strip_nl(uuid);
        fclose(f);
    }
    char id[96];
    if (uuid[0])
        snprintf(id, sizeof(id), "nativepipe-%s", uuid);
    else
        snprintf(id, sizeof(id), "nativepipe-%ld-%d", (long)time(NULL), (int)getpid());

    np_mkdir_p("/var/lib/cloud/data");
    np_mkdir_p("/var/lib/nativepipe");
    size_t n = strlen(id);
    np_write_file("/var/lib/cloud/data/instance-id", id, n, 0644);
    np_write_file("/var/lib/nativepipe/instance-id", id, n, 0644);
    fprintf(stderr, "[bootstrap] instance-id %s\n", id);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "[bootstrap] starting\n");

    int lock_fd = acquire_install_lock();
    if (lock_fd == -2)
        return 0;
    if (lock_fd < 0)
        die("could not lock guestd installation");

    bump_instance_id();

    char host_ver[NP_MAX_VERSION];
    host_ver[0] = '\0';
    int rc = np_agent_pull_file(NP_GUESTD_NAME, "", NP_INSTALLED_BIN, host_ver, sizeof(host_ver));
    if (rc == 2)
        die("host has no nativepipe-guestd");
    if (rc < 0)
        die("pull guestd failed");
    if (rc == 0) {
        np_write_version(host_ver);
        fprintf(stderr, "[bootstrap] installed guestd %s\n", host_ver);
    }

    if (!valid_guestd_elf())
        die("installed guestd is not a valid ELF");

    fprintf(stderr, "[bootstrap] %s --provision\n", NP_INSTALLED_BIN);
    char *args[] = {NP_INSTALLED_BIN, "--provision", NULL};
    rc = np_run(args);
    if (rc == 0) {
        static const char done[] = "ok\n";
        np_write_file(BOOTSTRAP_DONE, done, sizeof(done) - 1, 0600);
    }
    close(lock_fd);
    return rc;
}
