#define _GNU_SOURCE
#undef NDEBUG
#include "root.h"
#include "np_file_rpc.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int child(int argc, char **argv) {
    assert(getpid() == 1);
    assert(argc == 4 && !strcmp(argv[2], "spaces and $(no-shell)"));
    char input[64] = {0};
    assert(read(0, input, sizeof(input)) == 5 && !strcmp(input, "hello"));
    assert(access("/proc/self/status", R_OK) == 0);
    assert(access("/dev/null", W_OK) == 0);
    assert(access("/run/systemd/private", F_OK) != 0);
    if (!strcmp(argv[3], "orphan")) {
        pid_t orphan = fork(); assert(orphan >= 0);
        if (!orphan) { for (;;) pause(); }
        return 0; /* PID namespace exit must also reap the orphan. */
    }
    return 42;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return child(argc, argv);
    char path[] = "/tmp/nativepipe-root-test.XXXXXX";
    assert(mkdtemp(path));
    int root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); assert(root >= 0);
    assert(np_root_mkdir(root, "/usr/bin", 0755) == 0);
    assert(np_root_link(root, "/bin", "/usr/bin") == 0);
    assert(np_root_write(root, "/bin/marker", "inside", 6, 0600) == 0);
    char data[32];
    assert(np_root_read(root, "/usr/bin/marker", data, sizeof(data)) == 6);
    assert(!strcmp(data, "inside"));
    assert(np_root_mkdir(root, "/etc", 0755) == 0);
    assert(np_root_link(root, "/etc/escape", "/tmp/np-outside") == 0);
    assert(np_root_write(root, "/etc/escape/marker", "safe", 4, 0600) < 0);
    assert(np_root_mkdir(root, "/tmp/np-outside", 0755) == 0);
    assert(np_root_write(root, "/etc/escape/marker", "safe", 4, 0600) == 0);
    assert(np_root_read(root, "/tmp/np-outside/marker", data, sizeof(data)) == 4);
    assert(access("/tmp/np-outside/marker", F_OK) < 0);
    int self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC); assert(self >= 0);
    assert(np_root_copy(root, "/bin/test", self, 0755) == 0);
    close(self);
    char *args[] = {"/bin/test", "--child", "spaces and $(no-shell)", "error", NULL};
    assert(np_root_run(root, args, "hello", 5) == 42);
    args[3] = "orphan";
    for (unsigned i = 0; i < 4; i++) assert(np_root_run(root, args, "hello", 5) == 0);
    /* Runtime mounts must disappear when their owning operation exits. */
    assert(np_file_open(root, "/proc/self/status", O_RDONLY, 0) < 0);
    assert(np_file_open(root, "/dev/null", O_RDONLY, 0) < 0);
    puts("installer root: rooted symlinks, argv/stdin, exit status, namespace and mount cleanup PASS");
    assert(np_root_unlink(root, "/bin/test") == 0);
    assert(np_root_unlink(root, "/bin/marker") == 0);
    assert(np_root_unlink(root, "/bin") == 0);
    assert(np_root_unlink(root, "/etc/escape") == 0);
    assert(np_root_unlink(root, "/tmp/np-outside/marker") == 0);
    const char *directories[] = {"usr/bin", "usr", "etc", "dev", "proc", "sys", "run/rosetta", "run", "tmp/np-outside", "tmp"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        int rc = unlinkat(root, directories[i], AT_REMOVEDIR);
        assert(rc == 0 || errno == ENOENT);
    }
    close(root);
    assert(rmdir(path) == 0);
    return 0;
}
