#ifndef NP_INSTALL_ERROR_H
#define NP_INSTALL_ERROR_H

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* One atomic, bounded record on the private installer-to-PID-1 pipe. This is
 * not the host protocol. Package processes do not inherit the descriptor. */
struct np_install_error {
    int32_t code;
    char message[508];
};

static inline int np_install_error_fd(void) {
    const char *value = getenv("NP_INSTALL_ERROR_FD");
    if (!value || !*value) return -1;
    char *end;
    long fd = strtol(value, &end, 10);
    return !*end && fd > STDERR_FILENO && fd <= INT_MAX ? (int)fd : -1;
}

static inline void np_install_report_error(int code, const char *message) {
    int saved = errno;
    int fd = np_install_error_fd();
    if (fd >= 0) {
        struct np_install_error report = {.code = code > 0 ? code : EPROTO};
        snprintf(report.message, sizeof(report.message), "%s", message);
        /* Nonblocking, <= PIPE_BUF. Error reporting must never stall cleanup. */
        ssize_t written;
        do { written = write(fd, &report, sizeof(report)); } while (written < 0 && errno == EINTR);
    }
    errno = saved;
}
#endif
