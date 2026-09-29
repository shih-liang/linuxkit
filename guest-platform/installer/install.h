#ifndef NP_INSTALL_H
#define NP_INSTALL_H

#include "distro.h"
#include <sys/types.h>

struct np_install {
    const struct np_distribution *distribution;
    int root;
    int payload;
    int translated;
    int rosetta;
    int developer;
    int wine;
    int steam;
    int amd64;
    char username[33];
    char password[256];
    uid_t uid;
    gid_t gid;
};

int np_install_packages(struct np_install *install);
int np_install_package_set(struct np_install *install, int refresh, const char *names);
int np_install_software(struct np_install *install);
int np_install_amd64_entry(struct np_install *native, struct np_install *translated);
int np_install_account(struct np_install *install);
int np_install_configure(struct np_install *install);
int np_install_guest(struct np_install *install);
int np_install_program(int root, const char *name, char path[256]);

#endif
