#include "environment_config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct builder {
    uint8_t data[4096];
    size_t len;
};

static void fail(const char *message) {
    fprintf(stderr, "environment_config_test: %s\n", message);
    exit(1);
}

static void check(int condition, const char *message) {
    if (!condition)
        fail(message);
}

static void put8(struct builder *b, uint8_t value) {
    check(b->len < sizeof(b->data), "fixture overflow");
    b->data[b->len++] = value;
}

static void put16(struct builder *b, uint16_t value) {
    put8(b, (uint8_t)value);
    put8(b, (uint8_t)(value >> 8));
}

static void put32(struct builder *b, uint32_t value) {
    for (unsigned i = 0; i < 4; i++)
        put8(b, (uint8_t)(value >> (i * 8)));
}

static void put64(struct builder *b, uint64_t value) {
    for (unsigned i = 0; i < 8; i++)
        put8(b, (uint8_t)(value >> (i * 8)));
}

static void putstr(struct builder *b, const char *value) {
    size_t n = strlen(value);
    check(n > 0 && n < 256, "bad fixture string");
    put16(b, (uint16_t)n);
    for (size_t i = 0; i < n; i++)
        put8(b, (uint8_t)value[i]);
}

static void empty_match(struct builder *b) {
    put16(b, 0);
}

static void one_match(struct builder *b, const char *value) {
    put16(b, 1);
    putstr(b, value);
}

static void policy(struct builder *b, uint16_t wire, unsigned methods,
                   unsigned flags, unsigned administrator_groups,
                   uint8_t getty, uint8_t service, const char *shell) {
    put32(b, methods);
    put32(b, flags);
    if (wire >= 2)
        put32(b, administrator_groups);
    put8(b, getty);
    put8(b, service);
    put16(b, 1);
    putstr(b, shell);
}

static struct builder catalog(uint16_t wire, unsigned alpine_administrator_groups) {
    struct builder b;
    memset(&b, 0, sizeof(b));
    memcpy(b.data, "NPEC", 4);
    b.len = 4;
    put16(&b, wire);
    put16(&b, 2);
    put64(&b, 7);

    putstr(&b, "generic-systemd");
    put32(&b, 10);
    empty_match(&b);
    empty_match(&b);
    one_match(&b, "systemd");
    empty_match(&b);
    policy(&b, wire, NP_CONSOLE_METHOD_GRUB, 0, 0,
           NP_GETTY_SYSTEMD, NP_SERVICE_SYSTEMD, "/bin/sh");

    putstr(&b, "alpine-openrc");
    put32(&b, 300);
    one_match(&b, "alpine");
    empty_match(&b);
    one_match(&b, "openrc");
    empty_match(&b);
    policy(&b, wire, NP_CONSOLE_METHOD_EXTLINUX | NP_CONSOLE_METHOD_INITTAB,
           NP_ENV_DISABLE_UNAVAILABLE_GETTYS | NP_ENV_CLOUD_INIT_RECOVERY,
           alpine_administrator_groups,
           NP_GETTY_INITTAB, NP_SERVICE_OPENRC, "/bin/ash");
    return b;
}

int main(void) {
    struct builder b = catalog(2, NP_ADMINISTRATOR_GROUP_WHEEL);
    struct np_environment_policy selected;
    struct np_environment_facts alpine = {
        .os_id = "alpine",
        .os_id_like = "",
        .version_id = "3.24.1",
        .init_system = "openrc",
        .architecture = "aarch64",
    };
    check(np_environment_catalog_select_profile(
              b.data, b.len, &alpine, "alpine-openrc", &selected) == 0,
          "host-selected Alpine profile was rejected");
    check(strcmp(selected.profile_id, "alpine-openrc") == 0,
          "wrong Alpine profile");
    check(selected.revision == 7, "revision not decoded");
    check(selected.service_adapter == NP_SERVICE_OPENRC, "wrong service adapter");
    check(strcmp(selected.login_shells[0], "/bin/ash") == 0, "wrong shell order");
    check(selected.administrator_groups == NP_ADMINISTRATOR_GROUP_WHEEL,
          "administrator group policy was not decoded");
    errno = 0;
    check(np_environment_catalog_select_profile(
              b.data, b.len, &alpine, "generic-systemd", &selected) < 0,
          "host profile conflicting with live facts was accepted");
    check(errno == ENOENT, "conflicting host profile returned wrong error");

    struct np_environment_facts ubuntu = {
        .os_id = "ubuntu",
        .os_id_like = "debian",
        .version_id = "26.04",
        .init_system = "systemd",
        .architecture = "aarch64",
    };
    check(np_environment_catalog_select_profile(
              b.data, b.len, &ubuntu, "generic-systemd", &selected) == 0,
          "host-selected generic systemd profile failed");
    check(strcmp(selected.profile_id, "generic-systemd") == 0,
          "wrong generic profile");

    struct builder legacy = catalog(1, NP_ADMINISTRATOR_GROUP_WHEEL);
    check(np_environment_catalog_select_profile(
              legacy.data, legacy.len, &alpine, "alpine-openrc", &selected) == 0,
          "legacy v1 catalog was rejected");
    check(selected.administrator_groups == 0,
          "legacy catalog unexpectedly granted an administrator group");

    struct builder unsafe = catalog(2, 1u << 31);
    errno = 0;
    check(np_environment_catalog_select_profile(
              unsafe.data, unsafe.len, &alpine, "alpine-openrc", &selected) < 0,
          "unknown administrator group bit was accepted");
    check(errno == EPROTO, "unknown administrator group returned wrong error");

    errno = 0;
    check(np_environment_catalog_select_profile(
              b.data, b.len - 1, &alpine, "alpine-openrc", &selected) < 0,
          "truncated catalog accepted");
    check(errno == EPROTO, "truncated catalog returned wrong error");

    np_environment_fallback_policy(&selected);
    check(strcmp(selected.profile_id, "discovered-linux") == 0,
          "fallback missing");
    check(selected.console_methods & NP_CONSOLE_METHOD_INITTAB,
          "fallback lost inittab discovery");
    puts("environment_config_test: ok");
    return 0;
}
