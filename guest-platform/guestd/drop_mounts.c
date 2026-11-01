#define _GNU_SOURCE
#include "drop_mounts.h"
#include "np.h"
#include "np_drop_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* Mount points live under root-owned /run/nativepipe, never in a directory
 * the user controls, so no symbolic link can redirect where root mounts. */
static int root_owned_directory(const char *path) {
    struct stat st;
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return -1;
    if (lstat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) { errno = EPERM; return -1; }
    return 0;
}

static int mount_for(uid_t uid, gid_t gid, char *path, size_t capacity) {
    if (root_owned_directory("/run/nativepipe") < 0 || root_owned_directory(NP_DROP_MOUNT_ROOT) < 0) return -1;
    if ((size_t)snprintf(path, capacity, "%s/%u", NP_DROP_MOUNT_ROOT, (unsigned)uid) >= capacity) {
        errno = ENAMETOOLONG; return -1;
    }
    /* The newest session wins: a previous daemon's mount (alive or aborted)
     * is detached first, and its open files keep their old connection. Never
     * look inside it: without allow_other the kernel refuses even root, so
     * lstat/stat on the mount point fail with EACCES. umount2 only resolves
     * the mount point; EINVAL means nothing is mounted there any more. */
    for (int i = 0;; i++) {
        if (umount2(path, MNT_DETACH | UMOUNT_NOFOLLOW) == 0) {
            if (i == 8) { errno = EBUSY; return -1; }
            continue;
        }
        if (errno == EINVAL || errno == ENOENT) break;
        return -1;
    }
    if (root_owned_directory(path) < 0) return -1;
    int fuse = open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if (fuse < 0) return -1;
    char options[160];
    snprintf(options, sizeof(options),
             "fd=%d,rootmode=40000,user_id=%u,group_id=%u,default_permissions,max_read=%u",
             fuse, (unsigned)uid, (unsigned)gid, NP_DROP_MAX_READ);
    if (mount(NP_DROP_FS_NAME, path, "fuse", MS_NOSUID | MS_NODEV, options) < 0) {
        int error = errno; close(fuse); errno = error; return -1;
    }
    return fuse;
}

static void answer(int client) {
    struct ucred peer;
    socklen_t size = sizeof(peer);
    char request[4];
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 || size != sizeof(peer) ||
        np_read_full(client, request, sizeof(request)) < 0 || memcmp(request, "NPDM", 4)) return;
    char path[128] = "";
    int fuse = -1;
    uint32_t status = 0;
    /* A root session would own a root-accessible mount; sessions never run as root. */
    if (peer.uid == 0) status = EPERM;
    else if ((fuse = mount_for(peer.uid, peer.gid, path, sizeof(path))) < 0) status = (uint32_t)errno;
    size_t length = fuse >= 0 ? strlen(path) : 0;
    unsigned char reply[10 + sizeof(path)];
    memcpy(reply, "NPDM", 4);
    for (int i = 0; i < 4; i++) reply[4 + i] = (unsigned char)(status >> (8 * i));
    reply[8] = (unsigned char)length; reply[9] = (unsigned char)(length >> 8);
    memcpy(reply + 10, path, length);
    union { struct cmsghdr header; char space[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct iovec vector = {reply, 10 + length};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    if (fuse >= 0) {
        message.msg_control = control.space; message.msg_controllen = sizeof(control.space);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &fuse, sizeof(int));
    }
    if (sendmsg(client, &message, MSG_NOSIGNAL) < 0) fprintf(stderr, "drops: reply: %s\n", strerror(errno));
    /* The session now holds the only daemon end; ours must not keep it alive. */
    if (fuse >= 0) close(fuse);
}

static void *serve(void *argument) {
    int listener = (int)(intptr_t)argument;
    for (;;) {
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) { if (errno != EINTR) usleep(100000); continue; }
        /* One short exchange; a stalled client cannot hold the listener. */
        struct timeval timeout = {5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        answer(client);
        close(client);
    }
    return NULL;
}

int np_drop_mounts_start(void) {
    if (root_owned_directory("/run/nativepipe") < 0) return -1;
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, NP_DROP_MOUNT_SOCKET);
    unlink(NP_DROP_MOUNT_SOCKET);
    /* Every user may ask, but only for a mount owned by their own UID. */
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        chmod(NP_DROP_MOUNT_SOCKET, 0666) < 0 || listen(listener, 8) < 0) {
        int error = errno; close(listener); errno = error; return -1;
    }
    pthread_t thread;
    int error = pthread_create(&thread, NULL, serve, (void *)(intptr_t)listener);
    if (error) { close(listener); errno = error; return -1; }
    pthread_detach(thread);
    return 0;
}
