#ifndef NATIVEPIPE_ENVIRONMENT_CONFIG_H
#define NATIVEPIPE_ENVIRONMENT_CONFIG_H

#include "console_config.h"

#include <stddef.h>
#include <stdint.h>

#define NP_ENV_CATALOG_NAME "environment/catalog.npec"
#define NP_ENV_CATALOG_CACHE "/var/lib/nativepipe/environment/catalog.npec"
#define NP_ENV_PROFILE_CACHE "/var/lib/nativepipe/environment/profile"
#define NP_ENV_MAX_CATALOG (1024u * 1024u)
#define NP_ENV_MAX_PROFILE_ID 128
#define NP_ENV_MAX_SHELLS 8
#define NP_ENV_MAX_SHELL_PATH 128

enum np_environment_flag {
    NP_ENV_DISABLE_UNAVAILABLE_GETTYS = 1u << 0,
    NP_ENV_CLOUD_INIT_RECOVERY = 1u << 1,
    NP_ENV_REPAIR_CLOUD_INIT_NETWORK = 1u << 2,
};

enum np_getty_adapter {
    NP_GETTY_AUTO = 0,
    NP_GETTY_SYSTEMD = 1,
    NP_GETTY_INITTAB = 2,
    NP_GETTY_NONE = 3,
};

enum np_service_adapter {
    NP_SERVICE_AUTO = 0,
    NP_SERVICE_SYSTEMD = 1,
    NP_SERVICE_OPENRC = 2,
    NP_SERVICE_PROCESS = 3,
};

/* The signed catalog selects only these compiled account-role adapters. It
 * cannot name arbitrary groups such as disk, docker, or an injected command. */
enum np_administrator_group {
    NP_ADMINISTRATOR_GROUP_WHEEL = 1u << 0,
    NP_ADMINISTRATOR_GROUP_SUDO = 1u << 1,
};

struct np_environment_facts {
    const char *os_id;
    const char *os_id_like;
    const char *version_id;
    const char *init_system;
    const char *architecture;
};

struct np_environment_policy {
    uint64_t revision;
    char profile_id[NP_ENV_MAX_PROFILE_ID];
    unsigned console_methods;
    unsigned flags;
    unsigned administrator_groups;
    unsigned char getty_adapter;
    unsigned char service_adapter;
    size_t shell_count;
    char login_shells[NP_ENV_MAX_SHELLS][NP_ENV_MAX_SHELL_PATH];
};

/* Select the profile chosen by the host, while still checking its match
 * constraints against the live guest facts. */
int np_environment_catalog_select_profile(const void *data, size_t len,
                                           const struct np_environment_facts *facts,
                                           const char *profile_id,
                                           struct np_environment_policy *policy);

/* Distribution-neutral discovery fallback used when no host catalog exists. */
void np_environment_fallback_policy(struct np_environment_policy *policy);

#endif
