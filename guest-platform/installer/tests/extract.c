#include "rootfs.h"
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    enum np_rootfs_format format;
    if (!strcmp(argv[3], "tar")) format = NP_ROOTFS_TAR;
    else if (!strcmp(argv[3], "oci")) format = NP_ROOTFS_OCI;
    else if (!strcmp(argv[3], "arch")) format = NP_ROOTFS_ARCH_BOOTSTRAP;
    else return 2;
    return np_rootfs_extract(argv[1], argv[2], format, argv[4]) == 0 ? 0 : 1;
}
