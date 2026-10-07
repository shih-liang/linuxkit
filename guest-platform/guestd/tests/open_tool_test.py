#!/usr/bin/env python3
"""Run the actual optional installer worker with temporary files and mocked pulls."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


source = Path(__file__).resolve().parents[1] / "nativepipe-guestd.c"
text = source.read_text()
worker = text.split("enum { NP_OPEN_NEEDED, NP_OPEN_INSTALLING, NP_OPEN_READY };", 1)[1]
worker = "enum { NP_OPEN_NEEDED, NP_OPEN_INSTALLING, NP_OPEN_READY };" + worker.split(
    "/* Pull a complete display generation", 1)[0]

fixture = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define NP_MAX_VERSION 128
#define NP_OPEN_NAME "nativepipe-open"
static atomic_int pulls, released, fail_create;
static void logmsg(const char *message) { (void)message; }
static int np_mkdir_p(const char *path) { assert(!strcmp(path, "/usr/local/bin")); return 0; }
static void write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(close(fd) == 0);
}
static int np_agent_pull_file_mode_n(const char *name, const char *current, const char *path,
                                     int mode, char *version, size_t cap, int retries) {
    assert(!strcmp(name, NP_OPEN_NAME) && !current[0] && mode == 0755 && retries == 2);
    assert(cap >= 4);
    int attempt = atomic_fetch_add(&pulls, 1) + 1;
    while (!atomic_load(&released)) usleep(1000);
    write_text(path, attempt == 1 ? "partial" : "new-open");
    assert(chmod(path, mode) == 0);
    snprintf(version, cap, "0.2");
    return attempt == 1 ? -1 : 0;
}
static int create_thread(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*entry)(void *), void *arg) {
    if (atomic_load(&fail_create)) return EAGAIN;
    return pthread_create(thread, attr, entry, arg);
}
#define pthread_create create_thread
'''

checks = r'''
static void wait_for(atomic_int *state, int expected) {
    for (int i = 0; i < 5000 && atomic_load(state) != expected; i++) usleep(1000);
    assert(atomic_load(state) == expected);
}
static void check_text(const char *expected) {
    int fd = open(NP_INSTALLED_OPEN, O_RDONLY);
    char value[32] = {0};
    assert(fd >= 0 && read(fd, value, sizeof(value) - 1) > 0 && close(fd) == 0);
    assert(!strcmp(value, expected));
}
int main(void) {
    write_text(NP_INSTALLED_OPEN, "previous");
    atomic_store(&fail_create, 1);
    ensure_open_tool_async();
    assert(atomic_load(&open_tool_state) == NP_OPEN_NEEDED && !atomic_load(&pulls));
    atomic_store(&fail_create, 0);
    ensure_open_tool_async();
    wait_for(&pulls, 1);
    for (int i = 0; i < 100; i++) ensure_open_tool_async();
    assert(atomic_load(&pulls) == 1);
    atomic_store(&released, 1);
    wait_for(&open_tool_state, NP_OPEN_NEEDED);
    check_text("previous");
    ensure_open_tool_async();
    wait_for(&open_tool_state, NP_OPEN_READY);
    assert(atomic_load(&pulls) == 2);
    check_text("new-open");
    for (int i = 0; i < 100; i++) ensure_open_tool_async();
    assert(atomic_load(&pulls) == 2);
    puts("open tool installer: nonblocking single worker, retry and atomic replacement PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="nativepipe-open-install-test-") as folder:
    root = Path(folder)
    program, executable = root / "test.c", root / "test"
    program.write_text(fixture + "\n#define NP_INSTALLED_OPEN " + json.dumps(str(root / "np-open"))
                       + "\n" + worker + checks)
    compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
    subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread",
                               str(program), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=15)
