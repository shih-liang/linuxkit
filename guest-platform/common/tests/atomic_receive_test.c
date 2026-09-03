#define _GNU_SOURCE
#include "np.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PAYLOAD_SIZE (1024u * 1024u)

struct transfer {
    int read_fd;
    int write_fd;
    const char *destination;
    unsigned char fill;
    int receive_rc;
    int write_rc;
};

static void fail(const char *message) {
    fprintf(stderr, "atomic_receive_test: %s\n", message);
    exit(1);
}

static void check(int condition, const char *message) {
    if (!condition)
        fail(message);
}

static void *receive_payload(void *opaque) {
    struct transfer *t = opaque;
    t->receive_rc = np_agent_recv_payload_file(
        t->read_fd, PAYLOAD_SIZE, t->destination, 0755);
    close(t->read_fd);
    return NULL;
}

static void *write_payload(void *opaque) {
    struct transfer *t = opaque;
    unsigned char block[4096];
    memset(block, t->fill, sizeof(block));
    size_t left = PAYLOAD_SIZE;
    t->write_rc = 0;
    while (left > 0) {
        size_t chunk = left > sizeof(block) ? sizeof(block) : left;
        if (np_write_full(t->write_fd, block, chunk) < 0) {
            t->write_rc = -1;
            break;
        }
        left -= chunk;
        sched_yield();
    }
    close(t->write_fd);
    return NULL;
}

static void verify_complete_file(const char *path) {
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "destination missing");
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == PAYLOAD_SIZE,
          "destination has a partial size");
    unsigned char expected = 0;
    unsigned char block[8192];
    size_t total = 0;
    while (total < PAYLOAD_SIZE) {
        ssize_t n = read(fd, block, sizeof(block));
        check(n > 0, "destination read failed");
        if (total == 0) {
            expected = block[0];
            check(expected == 'A' || expected == 'B', "destination has unknown payload");
        }
        for (ssize_t i = 0; i < n; i++)
            check(block[i] == expected, "concurrent payloads were mixed");
        total += (size_t)n;
    }
    close(fd);
}

static void verify_no_staging_files(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    check(dir != NULL, "test directory missing");
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        check(strstr(entry->d_name, ".new.") == NULL, "staging file leaked");
    }
    closedir(dir);
}

int main(void) {
    char root[] = "/tmp/nativepipe-atomic-test.XXXXXX";
    check(mkdtemp(root) != NULL, "mkdtemp failed");
    char destination[PATH_MAX];
    check(snprintf(destination, sizeof(destination), "%s/nativepipe-guestd", root) <
              (int)sizeof(destination),
          "destination path too long");

    for (int iteration = 0; iteration < 20; iteration++) {
        int sockets[2][2];
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets[0]) == 0, "socketpair A failed");
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets[1]) == 0, "socketpair B failed");
        struct transfer transfers[2] = {
            {.read_fd = sockets[0][0], .write_fd = sockets[0][1],
             .destination = destination, .fill = 'A', .receive_rc = -1, .write_rc = -1},
            {.read_fd = sockets[1][0], .write_fd = sockets[1][1],
             .destination = destination, .fill = 'B', .receive_rc = -1, .write_rc = -1},
        };
        pthread_t receivers[2];
        pthread_t writers[2];
        for (int i = 0; i < 2; i++)
            check(pthread_create(&receivers[i], NULL, receive_payload, &transfers[i]) == 0,
                  "receiver thread failed");
        for (int i = 0; i < 2; i++)
            check(pthread_create(&writers[i], NULL, write_payload, &transfers[i]) == 0,
                  "writer thread failed");
        for (int i = 0; i < 2; i++) {
            pthread_join(writers[i], NULL);
            pthread_join(receivers[i], NULL);
            check(transfers[i].write_rc == 0, "payload write failed");
            check(transfers[i].receive_rc == 0, "payload receive failed");
        }
        verify_complete_file(destination);
        verify_no_staging_files(root);
    }

    unlink(destination);
    rmdir(root);
    puts("atomic_receive_test: ok");
    return 0;
}
