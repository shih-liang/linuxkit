#include "root.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* Diagnostic runner for distribution integration tests; never installed. */
int main(int argc, char **argv) {
    if (argc < 3 || geteuid() != 0) return 2;
    int root = open(argv[1], O_RDONLY | O_DIRECTORY);
    if (root < 0) { perror(argv[1]); return 2; }
    int rc = np_root_run(root, argv + 2, NULL, 0);
    close(root);
    return rc < 0 ? 1 : rc;
}
