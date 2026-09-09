#define _GNU_SOURCE
#include "exec_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define HEADER 12u
#define MAX_INPUT (1u << 20)
#define DATA 1u
#define RESIZE 2u
#define EXIT 3u
#define END_INPUT 4u
#define STDERR_DATA 5u

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (i * 8));
}
static void header(uint8_t *p, uint8_t type, uint32_t count) {
    memcpy(p, "NPXT", 4); p[4] = 1; p[5] = type; p[6] = p[7] = 0; put32(p + 8, count);
}
static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 ? fcntl(fd, F_SETFL, flags | O_NONBLOCK) : -1;
}
static int retry_io(void) { return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK; }

void np_exec_cancel(pid_t child) {
    if (child <= 0) return;
    /* The child creates a session before exec. The positive PID also covers
     * cancellation before setsid; never signal the agent's process group. */
    kill(-child, SIGTERM); kill(child, SIGTERM);
    for (unsigned i = 0; i < 50; i++) {
        siginfo_t info = {0};
        int result = waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG | WNOWAIT);
        if (result < 0 && errno == ECHILD) return;
        if (result == 0 && info.si_pid == child) break;
        struct timespec delay = {.tv_nsec = 20000000}; nanosleep(&delay, NULL);
    }
    kill(-child, SIGKILL); kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

struct stream {
    uint8_t input[HEADER + MAX_INPUT];
    size_t input_size, input_sent;
    uint8_t output[HEADER + 8192];
    size_t output_size, output_sent;
    int input_ended, output_ended, error_ended, exiting, exited, status, next_output;
};

/* Never block inside a pipe/socket write: the same poll loop must remain able
 * to drain stdout/stderr, process a disconnect and forward the opposite side. */
static int consume_input(struct stream *s, int fd, int terminal) {
    while (s->input_size >= HEADER) {
        uint8_t *p = s->input;
        if (memcmp(p, "NPXT", 4) || p[4] != 1 || p[6] || p[7]) { errno = EPROTO; return -1; }
        uint32_t length = u32(p + 8);
        if (length > MAX_INPUT) { errno = EMSGSIZE; return -1; }
        if (s->input_size < HEADER + length) return 0;
        if (p[5] == DATA) {
            if (s->input_ended) { errno = EPROTO; return -1; }
            while (s->input_sent < length) {
                ssize_t n = write(fd, p + HEADER + s->input_sent, length - s->input_sent);
                if (n < 0 && retry_io()) return 0;
                if (n <= 0) return -1;
                s->input_sent += (size_t)n;
            }
        } else if (p[5] == RESIZE && length == 8) {
            if (terminal) {
                uint32_t cols = u32(p + HEADER), rows = u32(p + HEADER + 4);
                struct winsize size = {.ws_col = (unsigned short)cols, .ws_row = (unsigned short)rows};
                if (cols > UINT16_MAX || rows > UINT16_MAX) { errno = EINVAL; return -1; }
                if (ioctl(fd, TIOCSWINSZ, &size) < 0) return -1;
            }
        } else if (p[5] == END_INPUT && !length && !s->input_ended) {
            if (terminal) {
                struct termios attrs;
                if (tcgetattr(fd, &attrs) < 0) return -1;
                unsigned char eof = attrs.c_cc[VEOF];
                if (eof != _POSIX_VDISABLE) {
                    ssize_t n = write(fd, &eof, 1);
                    if (n < 0 && retry_io()) return 0;
                    if (n != 1) return -1;
                }
            } else if (shutdown(fd, SHUT_WR) < 0 && errno != ENOTCONN) return -1;
            s->input_ended = 1;
        } else { errno = EPROTO; return -1; }
        size_t used = HEADER + length;
        memmove(p, p + used, s->input_size - used);
        s->input_size -= used; s->input_sent = 0;
    }
    return 0;
}

static int read_output(struct stream *s, int fd, int error, int terminal) {
    ssize_t n = read(fd, s->output + HEADER, sizeof(s->output) - HEADER);
    if (n > 0) {
        header(s->output, error ? STDERR_DATA : DATA, (uint32_t)n);
        s->output_size = HEADER + (size_t)n; s->output_sent = 0;
        return 0;
    }
    if (n < 0 && retry_io()) return 0;
    /* A PTY reports EIO after its last slave closes, not read()==0. */
    if (n < 0 && !(terminal && errno == EIO)) return -1;
    if (error) s->error_ended = 1; else s->output_ended = 1;
    return 0;
}

int np_exec_stream(int socket, int io, int error, pid_t child, int terminal) {
    struct stream *s = calloc(1, sizeof(*s));
    int result = -1;
    if (!s || nonblocking(socket) < 0 || nonblocking(io) < 0 ||
        (error >= 0 && nonblocking(error) < 0)) goto done;
    s->error_ended = error < 0;
    for (;;) {
        if (!s->exited) {
            siginfo_t info = {0};
            int waited = waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG | WNOWAIT);
            if (waited == 0 && info.si_pid == child) {
                s->exited = 1;
                s->status = info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
            } else if (waited < 0 && errno != EINTR) goto done;
        }
        if (!s->output_size && s->output_ended && s->error_ended && s->exited) {
            if (s->exiting) { result = 0; break; }
            uint32_t status = (uint32_t)s->status;
            header(s->output, EXIT, 4); put32(s->output + HEADER, status);
            s->output_size = HEADER + 4; s->output_sent = 0; s->exiting = 1;
        }
        if (s->exited) s->input_size = s->input_sent = 0;
        else if (consume_input(s, io, terminal) < 0) goto done;
        int pending_input = s->input_size >= HEADER && s->input_size >= HEADER + u32(s->input + 8);
        struct pollfd fds[3] = {
            {.fd = socket, .events = (s->input_size < sizeof(s->input) ? POLLIN : 0)
                                    | (s->output_size ? POLLOUT : 0)},
            {.fd = io, .events = ((!s->output_size && !s->output_ended) ? POLLIN : 0)
                                | (pending_input ? POLLOUT : 0)},
            {.fd = error, .events = (!s->output_size && !s->error_ended) ? POLLIN : 0},
        };
        /* Closed streams must not produce a hot POLLHUP loop while waiting
         * for a process which deliberately closed stdio before exiting. */
        if (!fds[1].events) fds[1].fd = -1;
        if (!fds[2].events) fds[2].fd = -1;
        int ready = poll(fds, 3, s->exited ? -1 : 100);
        if (ready < 0) { if (errno == EINTR) continue; goto done; }
        if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) goto done;
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(socket, s->input + s->input_size, sizeof(s->input) - s->input_size);
            if (n > 0) s->input_size += (size_t)n;
            else if (!n || !retry_io()) goto done;
        }
        if (s->output_size && (fds[0].revents & POLLOUT)) {
            ssize_t n = send(socket, s->output + s->output_sent, s->output_size - s->output_sent,
#ifdef MSG_NOSIGNAL
                             MSG_NOSIGNAL
#else
                             0
#endif
            );
            if (n > 0) {
                s->output_sent += (size_t)n;
                if (s->output_sent == s->output_size) s->output_size = s->output_sent = 0;
            } else if (!n || !retry_io()) goto done;
        }
        for (int j = 0; j < 2 && !s->output_size; j++) {
            int i = 1 + (s->next_output + j) % 2;
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (read_output(s, fds[i].fd, i == 2, terminal) < 0) goto done;
                if (s->output_size) s->next_output = i % 2;
            }
        }
    }
done:;
    int saved = errno;
    close(io); if (error >= 0) close(error);
    if (result < 0) np_exec_cancel(child);
    else while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    free(s); errno = saved;
    return result;
}
