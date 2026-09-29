#ifndef NP_INSTALL_H
#define NP_INSTALL_H

#include "distro.h"
#include <stddef.h>
#include <sys/types.h>

struct np_install {
    const struct np_distribution *distribution;
    int root;
    int payload;
    const char *architecture;
    int rosetta;
    int live; /* apps command on the running system; never repair its DNS. */
    char username[33];
    char password[256];
    uid_t uid;
    gid_t gid;
};

int np_install_packages(struct np_install *install);
int np_install_package_set(struct np_install *install, int refresh, const char *names);
int np_install_run(struct np_install *install, char *const argv[], const void *input, size_t length);
int np_install_account(struct np_install *install);
int np_install_configure(struct np_install *install);
int np_install_guest(struct np_install *install);
int np_install_program(int root, const char *name, char path[256]);

#endif
