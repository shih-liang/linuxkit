#define _POSIX_C_SOURCE 200809L

#include "environment_config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define NP_ENV_WIRE_VERSION_MIN 1u
#define NP_ENV_WIRE_VERSION 2u
#define NP_ENV_MAX_PROFILES 128u
#define NP_ENV_MAX_MATCH_VALUES 32u
#define NP_ENV_ALL_CONSOLE_METHODS                                               \
    (NP_CONSOLE_METHOD_GRUB | NP_CONSOLE_METHOD_LOADER_ENTRY |                  \
     NP_CONSOLE_METHOD_KERNEL_CMDLINE | NP_CONSOLE_METHOD_EXTLINUX |            \
     NP_CONSOLE_METHOD_INITTAB)
#define NP_ENV_ALL_FLAGS                                                         \
    (NP_ENV_DISABLE_UNAVAILABLE_GETTYS | NP_ENV_CLOUD_INIT_RECOVERY |           \
     NP_ENV_REPAIR_CLOUD_INIT_NETWORK)
#define NP_ENV_ALL_ADMINISTRATOR_GROUPS                                          \
    (NP_ADMINISTRATOR_GROUP_WHEEL | NP_ADMINISTRATOR_GROUP_SUDO)

struct reader {
    const uint8_t *data;
    size_t len;
    size_t off;
};

static int rd_u8(struct reader *r, uint8_t *out) {
    if (r->off >= r->len)
        return -1;
    *out = r->data[r->off++];
    return 0;
}

static int rd_u16(struct reader *r, uint16_t *out) {
    if (r->len - r->off < 2)
        return -1;
    const uint8_t *p = r->data + r->off;
    *out = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    r->off += 2;
    return 0;
}

static int rd_u32(struct reader *r, uint32_t *out) {
    if (r->len - r->off < 4)
        return -1;
    const uint8_t *p = r->data + r->off;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    r->off += 4;
    return 0;
}

static int rd_u64(struct reader *r, uint64_t *out) {
    if (r->len - r->off < 8)
        return -1;
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value |= (uint64_t)r->data[r->off + i] << (i * 8);
    r->off += 8;
    *out = value;
    return 0;
}

static int rd_string(struct reader *r, char *out, size_t cap) {
    uint16_t n = 0;
    if (rd_u16(r, &n) < 0 || n == 0 || (size_t)n >= cap || r->len - r->off < n)
        return -1;
    memcpy(out, r->data + r->off, n);
    out[n] = '\0';
    r->off += n;
    return 0;
}

static int token_list_contains(const char *list, const char *value) {
    if (!list || !value || !value[0])
        return 0;
    size_t value_len = strlen(value);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        size_t n = (size_t)(p - start);
        if (n == value_len && strncasecmp(start, value, n) == 0)
            return 1;
    }
    return 0;
}

enum match_kind {
    MATCH_IDENTITY,
    MATCH_VERSION_PREFIX,
    MATCH_INIT,
    MATCH_ARCH,
};

static int value_matches(enum match_kind kind, const char *value,
                         const struct np_environment_facts *facts) {
    const char *actual = NULL;
    switch (kind) {
    case MATCH_IDENTITY:
        return (facts->os_id && strcasecmp(value, facts->os_id) == 0) ||
               token_list_contains(facts->os_id_like, value);
    case MATCH_VERSION_PREFIX:
        actual = facts->version_id;
        return actual && strncmp(actual, value, strlen(value)) == 0;
    case MATCH_INIT:
        actual = facts->init_system;
        break;
    case MATCH_ARCH:
        actual = facts->architecture;
        break;
    }
    return actual && strcasecmp(actual, value) == 0;
}

static int rd_match_array(struct reader *r, enum match_kind kind,
                          const struct np_environment_facts *facts, int *matched) {
    uint16_t count = 0;
    if (rd_u16(r, &count) < 0 || count > NP_ENV_MAX_MATCH_VALUES)
        return -1;
    *matched = count == 0;
    for (uint16_t i = 0; i < count; i++) {
        char value[256];
        if (rd_string(r, value, sizeof(value)) < 0)
            return -1;
        if (value_matches(kind, value, facts))
            *matched = 1;
    }
    return 0;
}

static int catalog_select(const void *data, size_t len,
                          const struct np_environment_facts *facts,
                          const char *required_profile,
                          struct np_environment_policy *policy) {
    if (!data || !facts || !required_profile || !required_profile[0] || !policy ||
        len < 16 || len > NP_ENV_MAX_CATALOG) {
        errno = EINVAL;
        return -1;
    }
    struct reader r = {(const uint8_t *)data, len, 0};
    if (memcmp(r.data, "NPEC", 4) != 0) {
        errno = EPROTO;
        return -1;
    }
    r.off = 4;
    uint16_t wire = 0, profile_count = 0;
    uint64_t revision = 0;
    if (rd_u16(&r, &wire) < 0 || rd_u16(&r, &profile_count) < 0 ||
        rd_u64(&r, &revision) < 0 || wire < NP_ENV_WIRE_VERSION_MIN ||
        wire > NP_ENV_WIRE_VERSION || revision == 0 ||
        profile_count == 0 || profile_count > NP_ENV_MAX_PROFILES) {
        errno = EPROTO;
        return -1;
    }

    int found = 0;
    struct np_environment_policy best;
    memset(&best, 0, sizeof(best));

    for (uint16_t profile_index = 0; profile_index < profile_count; profile_index++) {
        struct np_environment_policy candidate;
        memset(&candidate, 0, sizeof(candidate));
        candidate.revision = revision;
        uint32_t priority_bits = 0;
        if (rd_string(&r, candidate.profile_id, sizeof(candidate.profile_id)) < 0 ||
            rd_u32(&r, &priority_bits) < 0) {
            errno = EPROTO;
            return -1;
        }
        (void)priority_bits;
        int identity = 0, version = 0, init = 0, arch = 0;
        if (rd_match_array(&r, MATCH_IDENTITY, facts, &identity) < 0 ||
            rd_match_array(&r, MATCH_VERSION_PREFIX, facts, &version) < 0 ||
            rd_match_array(&r, MATCH_INIT, facts, &init) < 0 ||
            rd_match_array(&r, MATCH_ARCH, facts, &arch) < 0) {
            errno = EPROTO;
            return -1;
        }
        uint32_t methods = 0, flags = 0, administrator_groups = 0;
        uint8_t getty = 0, service = 0;
        uint16_t shell_count = 0;
        if (rd_u32(&r, &methods) < 0 || rd_u32(&r, &flags) < 0 ||
            (wire >= 2 && rd_u32(&r, &administrator_groups) < 0) ||
            rd_u8(&r, &getty) < 0 || rd_u8(&r, &service) < 0 ||
            rd_u16(&r, &shell_count) < 0 || shell_count == 0 ||
            shell_count > NP_ENV_MAX_SHELLS) {
            errno = EPROTO;
            return -1;
        }
        candidate.console_methods = methods;
        candidate.flags = flags;
        candidate.administrator_groups = administrator_groups;
        candidate.getty_adapter = getty;
        candidate.service_adapter = service;
        candidate.shell_count = shell_count;
        for (uint16_t i = 0; i < shell_count; i++) {
            if (rd_string(&r, candidate.login_shells[i],
                          sizeof(candidate.login_shells[i])) < 0) {
                errno = EPROTO;
                return -1;
            }
        }
        if ((methods & ~NP_ENV_ALL_CONSOLE_METHODS) || methods == 0 ||
            (flags & ~NP_ENV_ALL_FLAGS) ||
            (administrator_groups & ~NP_ENV_ALL_ADMINISTRATOR_GROUPS) ||
            getty > NP_GETTY_NONE ||
            service > NP_SERVICE_PROCESS) {
            errno = EPROTO;
            return -1;
        }
        int requested = strcmp(candidate.profile_id, required_profile) == 0;
        if (requested && identity && version && init && arch) {
            if (found) {
                errno = EPROTO;
                return -1;
            }
            best = candidate;
            found = 1;
        }
    }
    if (r.off != r.len) {
        errno = EPROTO;
        return -1;
    }
    if (!found) {
        errno = ENOENT;
        return -1;
    }
    *policy = best;
    return 0;
}

int np_environment_catalog_select_profile(const void *data, size_t len,
                                           const struct np_environment_facts *facts,
                                           const char *profile_id,
                                           struct np_environment_policy *policy) {
    return catalog_select(data, len, facts, profile_id, policy);
}

void np_environment_fallback_policy(struct np_environment_policy *policy) {
    if (!policy)
        return;
    memset(policy, 0, sizeof(*policy));
    snprintf(policy->profile_id, sizeof(policy->profile_id), "discovered-linux");
    policy->console_methods = NP_ENV_ALL_CONSOLE_METHODS;
    policy->flags = NP_ENV_ALL_FLAGS;
    policy->getty_adapter = NP_GETTY_AUTO;
    policy->service_adapter = NP_SERVICE_AUTO;
    static const char *const shells[] = {
        "/bin/bash", "/usr/bin/bash", "/bin/ash", "/bin/sh",
    };
    policy->shell_count = sizeof(shells) / sizeof(shells[0]);
    for (size_t i = 0; i < policy->shell_count; i++)
        snprintf(policy->login_shells[i], sizeof(policy->login_shells[i]), "%s", shells[i]);
}
