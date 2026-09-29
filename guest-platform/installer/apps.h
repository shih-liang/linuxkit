#ifndef NP_INSTALL_APPS_H
#define NP_INSTALL_APPS_H
#include "install.h"
#define NP_MAX_APPLICATIONS 32
/* Canonical names are policy, not package names or shell input. */
struct np_application {
    const char *id, *name;
    const char *packages[4], *dependencies[4], *scripts[4];
};
const struct np_application *np_application_find(const char *id);
int np_application_supported(const struct np_application *app, const struct np_distribution *distro);
int np_applications_validate(const struct np_install *install, const char *const ids[], size_t count);
int np_applications_install(struct np_install *install, const char *const ids[], size_t count);
int np_applications_command(int argc, char **argv);
#endif
