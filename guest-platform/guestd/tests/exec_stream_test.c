#define _GNU_SOURCE
#include "exec_stream.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct session { int sock, io, err; pid_t child; int result; };
static void full(int fd, void *data, size_t length, int writing) {
    unsigned char *p = data;
    while (length) {
        ssize_t n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0); p += n; length -= (size_t)n;
    }
}
static void frame(int fd, uint8_t type, void *data, uint32_t length) {
    uint8_t h[12] = {'N','P','X','T',1,type};
    for (unsigned i = 0; i < 4; i++) h[8 + i] = (uint8_t)(length >> (8 * i));
    full(fd, h, sizeof(h), 1); if (length) full(fd, data, length, 1);
}
static void *serve(void *data) {
    struct session *s = data;
    s->result = np_exec_stream(s->sock, s->io, s->err, s->child, 0);
    shutdown(s->sock, SHUT_RDWR); close(s->sock); return NULL;
}
static int start(const char *command, struct session *s, pthread_t *thread) {
    int wire[2], io[2], error[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, wire) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, io) == 0); assert(pipe(error) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(wire[0]); close(wire[1]); close(io[0]); close(error[0]);
        assert(setsid() >= 0);
        assert(dup2(io[1], 0) >= 0 && dup2(io[1], 1) >= 0 && dup2(error[1], 2) >= 0);
        close(io[1]); close(error[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL); _exit(127);
    }
    close(io[1]); close(error[1]);
    *s = (struct session){.sock = wire[0], .io = io[0], .err = error[0], .child = child};
    assert(pthread_create(thread, NULL, serve, s) == 0); return wire[1];
}
static void *feed(void *data) {
    int fd = *(int *)data;
    unsigned char block[262144]; memset(block, 0x5a, sizeof(block));
    for (unsigned i = 0; i < 32; i++) frame(fd, 1, block, sizeof(block));
    frame(fd, 4, NULL, 0); return NULL;
}
static int receive(int fd, uint64_t *zeros, uint64_t *letters, unsigned *errors) {
    for (;;) {
        uint8_t h[12], bytes[8192]; full(fd, h, sizeof(h), 0);
        assert(!memcmp(h, "NPXT", 4));
        uint32_t length = h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
        assert(length <= sizeof(bytes)); full(fd, bytes, length, 0);
        if (h[5] == 3) { assert(length == 4); return bytes[0]; }
        if (h[5] == 5) { *errors += length; continue; }
        assert(h[5] == 1);
        for (unsigned i = 0; i < length; i++) {
            if (bytes[i] == 0) (*zeros)++; else { assert(bytes[i] == 0x5a); (*letters)++; }
        }
    }
}
int main(void) {
    signal(SIGPIPE, SIG_IGN); alarm(15);
    struct session s; pthread_t server, writer;
    int fd = start("dd if=/dev/zero bs=65536 count=128 2>/dev/null; cat; printf diagnostics >&2; exit 23", &s, &server);
    assert(pthread_create(&writer, NULL, feed, &fd) == 0);
    uint64_t zeros = 0, letters = 0; unsigned errors = 0;
    assert(receive(fd, &zeros, &letters, &errors) == 23);
    assert(zeros == 8 * 1024 * 1024 && letters == zeros && errors == strlen("diagnostics"));
    pthread_join(writer, NULL); close(fd); pthread_join(server, NULL); assert(s.result == 0);
    fd = start("exec 1>&- 2>&-; sleep 0.1; exit 7", &s, &server);
    assert(receive(fd, &zeros, &letters, &errors) == 7); close(fd); pthread_join(server, NULL);
    assert(s.result == 0);
    fd = start("trap '' TERM; sleep 60", &s, &server);
    shutdown(fd, SHUT_RDWR); close(fd); pthread_join(server, NULL); assert(s.result < 0);
    assert(waitpid(s.child, NULL, WNOHANG) == -1 && errno == ECHILD);
    alarm(0);
    puts("exec stream: simultaneous 8 MiB input/output, EOF, separate stderr, exit status and cancellation PASS");
}
