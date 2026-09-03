#define _GNU_SOURCE
/*
 * nativepipe-guestd — static guest agent.
 * Control plane: NPIP binary frames on vsock 1024 (ControlWire magics).
 * Guest-initiated artifact pull: NPAG on host-listened vsock 1029 (shared np.c).
 */
#include "np.h"
#include "console_config.h"
#include "cloud_init_config.h"
#include "environment_config.h"
#include "session_stack.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define CONSOLE_TTY "/dev/hvc0"
#define CONSOLE_INTEGRATION_STAMP "/var/lib/nativepipe/console-integration.version"
#define HOST_SHARE_TAG "lighthouse-shares"
#define HOST_SHARE_MOUNT "/mnt/lighthouse"

/* Baked in at build time from ./VERSION (−DNP_GUESTD_VERSION=…). */
#ifndef NP_GUESTD_VERSION
#define NP_GUESTD_VERSION "0.0.0-dev"
#endif
/*
 * Host scans the ELF for this exact prefix. Keep the record live so -O2/-s
 * cannot drop it; guest_version() reads NP_GUESTD_VERSION directly.
 */
__attribute__((used)) static const char np_guestd_version_record[] =
    "NPGV:" NP_GUESTD_VERSION;

#define NP_MAX_ARGS 32
#define NP_MAX_ENV 64
#define NP_MAX_READ (7u * 1024u * 1024u)
#define NP_MAX_STDERR (256u * 1024u)
#define NP_MAX_STDIN (64u * 1024u)
#define NP_LIST_NAME_BUDGET (64u * 1024u)
#define NP_LIST_HAS_MORE 1u
#define NP_LIST_NAME_TRUNC 2u
#define NP_ENTRY_CONT 1u
#define NP_ENTRY_INCOMPLETE 2u
#define NP_SETUSER_HAS_OLD 1u

static void logmsg(const char *msg) {
    fprintf(stderr, "[guestd] %s\n", msg);
}

static void *mount_host_shares(void *unused) {
    (void)unused;
    if (geteuid() != 0)
        return NULL;
    if (np_mkdir_p(HOST_SHARE_MOUNT) < 0) {
        logmsg("cannot create /mnt/lighthouse");
        return NULL;
    }
    for (int attempt = 0; attempt < 20; attempt++) {
        if (mount(HOST_SHARE_TAG, HOST_SHARE_MOUNT, "virtiofs",
                  MS_NODEV | MS_NOSUID, NULL) == 0) {
            logmsg("host shared folders mounted at /mnt/lighthouse");
            return NULL;
        }
        if (errno == EBUSY)
            return NULL;
        if (errno != ENODEV && errno != ENOENT && errno != EAGAIN) {
            fprintf(stderr, "[guestd] cannot mount host shared folders: %s\n",
                    strerror(errno));
            return NULL;
        }
        usleep(250000);
    }
    logmsg("host shared-folder device did not become ready");
    return NULL;
}

/* Request handlers may complete out of order. Serialize complete NPIP writes
 * so frame headers and payloads from different workers can never interleave. */
static pthread_mutex_t control_send_lock = PTHREAD_MUTEX_INITIALIZER;

static int control_send(int fd, const void *payload, size_t len) {
    pthread_mutex_lock(&control_send_lock);
    int rc = np_npip_send(fd, payload, len);
    pthread_mutex_unlock(&control_send_lock);
    return rc;
}

struct launch_req {
    char *exe;
    char *cwd;
    int have_cwd;
    char *args[NP_MAX_ARGS];
    int nargs;
    char *env_keys[NP_MAX_ENV];
    char *env_vals[NP_MAX_ENV];
    int nenv;
    uint8_t *stdin_data;
    size_t stdin_len;
    char *username;
};

struct guest_info {
    char agent_version[NP_MAX_VERSION];
    char kernel_release[128];
    char distro_name[128];
    char distro_version[128];
    char init_system[64];
    char os_id[128];
    char os_id_like[256];
    char architecture[128];
    char environment_profile[NP_ENV_MAX_PROFILE_ID];
    uint64_t environment_revision;
    int have_wayland;
};

static pthread_mutex_t environment_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t resource_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t session_stack_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t session_group_lock = PTHREAD_MUTEX_INITIALIZER;
static struct np_environment_policy environment_policy;
static int have_environment_policy;
static int session_stack_ready;

static void apply_cached_session_user_groups(void);

static void launch_req_clear(struct launch_req *r) {
    if (!r)
        return;
    free(r->exe);
    free(r->cwd);
    for (int i = 0; i < NP_MAX_ARGS; i++)
        free(r->args[i]);
    for (int i = 0; i < NP_MAX_ENV; i++) {
        free(r->env_keys[i]);
        free(r->env_vals[i]);
    }
    free(r->stdin_data);
    free(r->username);
    memset(r, 0, sizeof(*r));
}

static void os_release_get(const char *key, char *out, size_t cap) {
    snprintf(out, cap, "unknown");
    const char *paths[] = {"/etc/os-release", "/usr/lib/os-release", NULL};
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f)
            continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq = '\0';
            if (strcmp(line, key) != 0)
                continue;
            char *val = eq + 1;
            size_t n = strlen(val);
            while (n > 0 && (val[n - 1] == '\n' || val[n - 1] == '\r'))
                val[--n] = '\0';
            if (n >= 2 && val[0] == '"' && val[n - 1] == '"') {
                val[n - 1] = '\0';
                val++;
            }
            snprintf(out, cap, "%s", val);
            fclose(f);
            return;
        }
        fclose(f);
    }
}

static void detect_init(char *out, size_t cap) {
    snprintf(out, cap, "unknown");
    FILE *f = fopen("/proc/1/comm", "r");
    if (!f)
        return;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        snprintf(out, cap, "unknown");
        return;
    }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    if (strcmp(out, "openrc-init") == 0 ||
        (strcmp(out, "init") == 0 && np_path_exists("/run/openrc")))
        snprintf(out, cap, "openrc");
}

static int is_systemd(const char *init) {
    return strcmp(init, "systemd") == 0 ||
           (np_path_exists("/bin/systemctl") || np_path_exists("/usr/bin/systemctl"));
}

static int is_openrc(const char *init) {
    return strcmp(init, "openrc") == 0 ||
           (np_path_exists("/etc/init.d") &&
            (np_path_exists("/sbin/rc-update") || np_path_exists("/usr/sbin/rc-update")));
}

static void current_environment_policy(struct np_environment_policy *out) {
    pthread_mutex_lock(&environment_lock);
    if (have_environment_policy)
        *out = environment_policy;
    else
        np_environment_fallback_policy(out);
    pthread_mutex_unlock(&environment_lock);
}

static void set_environment_policy(const struct np_environment_policy *policy) {
    pthread_mutex_lock(&environment_lock);
    environment_policy = *policy;
    have_environment_policy = 1;
    pthread_mutex_unlock(&environment_lock);
}

static int has_host_environment_policy(void) {
    pthread_mutex_lock(&environment_lock);
    int have = have_environment_policy;
    pthread_mutex_unlock(&environment_lock);
    return have;
}

static int select_environment_catalog(const uint8_t *data, size_t len,
                                      const char *profile_id,
                                      struct np_environment_policy *selected) {
    char os_id[128], os_id_like[256], version_id[128], init[64], arch[128];
    os_release_get("ID", os_id, sizeof(os_id));
    os_release_get("ID_LIKE", os_id_like, sizeof(os_id_like));
    os_release_get("VERSION_ID", version_id, sizeof(version_id));
    detect_init(init, sizeof(init));
    struct utsname u;
    memset(&u, 0, sizeof(u));
    uname(&u);
    snprintf(arch, sizeof(arch), "%s", u.machine[0] ? u.machine : "unknown");
    struct np_environment_facts facts = {
        .os_id = os_id,
        .os_id_like = os_id_like,
        .version_id = version_id,
        .init_system = init,
        .architecture = arch,
    };
    return np_environment_catalog_select_profile(data, len, &facts, profile_id, selected);
}

static int read_environment_catalog_file(const char *path, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > NP_ENV_MAX_CATALOG) {
        int saved = errno ? errno : EFBIG;
        close(fd);
        errno = saved;
        return -1;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *data = malloc(len);
    if (!data) {
        close(fd);
        return -1;
    }
    if (np_read_full(fd, data, len) < 0) {
        int saved = errno;
        free(data);
        close(fd);
        errno = saved;
        return -1;
    }
    close(fd);
    *out = data;
    *out_len = len;
    return 0;
}

static int install_environment_catalog(const uint8_t *data, size_t len,
                                       const char *profile_id, int persist) {
    struct np_environment_policy selected;
    if (select_environment_catalog(data, len, profile_id, &selected) < 0)
        return -1;
    struct np_environment_policy current;
    current_environment_policy(&current);
    if (current.revision && selected.revision < current.revision) {
        errno = ESTALE;
        return -1;
    }
    if (persist) {
        static const char temporary[] = NP_ENV_CATALOG_CACHE ".new";
        if (np_write_file(temporary, data, len, 0644) < 0 ||
            rename(temporary, NP_ENV_CATALOG_CACHE) < 0) {
            int saved = errno;
            unlink(temporary);
            errno = saved;
            return -1;
        }
        static const char profile_temporary[] = NP_ENV_PROFILE_CACHE ".new";
        size_t profile_len = strlen(selected.profile_id);
        if (np_write_file(profile_temporary, selected.profile_id, profile_len, 0644) < 0 ||
            rename(profile_temporary, NP_ENV_PROFILE_CACHE) < 0) {
            int saved = errno;
            unlink(profile_temporary);
            errno = saved;
            return -1;
        }
    }
    set_environment_policy(&selected);
    fprintf(stderr, "[guestd] environment profile %s (catalog %llu)\n",
            selected.profile_id, (unsigned long long)selected.revision);
    return 0;
}

static void load_cached_environment_catalog(void) {
    uint8_t *data = NULL;
    size_t len = 0;
    char profile_id[NP_ENV_MAX_PROFILE_ID];
    profile_id[0] = '\0';
    FILE *profile = fopen(NP_ENV_PROFILE_CACHE, "r");
    if (profile) {
        if (!fgets(profile_id, sizeof(profile_id), profile))
            profile_id[0] = '\0';
        fclose(profile);
        size_t n = strlen(profile_id);
        while (n > 0 && (profile_id[n - 1] == '\n' || profile_id[n - 1] == '\r'))
            profile_id[--n] = '\0';
    }
    if (!profile_id[0]) {
        logmsg("no host-selected environment profile cached; using safe discovery fallback");
        return;
    }
    if (read_environment_catalog_file(NP_ENV_CATALOG_CACHE, &data, &len) == 0) {
        if (install_environment_catalog(data, len, profile_id, 0) < 0)
            logmsg("cached environment catalog is invalid; using discovery fallback");
        free(data);
    }
}

static void guest_version(char *out, size_t cap) {
    /* Source of truth is the stamp compiled into this ELF, not a sidecar file. */
    (void)np_guestd_version_record;
    snprintf(out, cap, "%s", NP_GUESTD_VERSION);
}

static void fill_guest_info(struct guest_info *gi) {
    memset(gi, 0, sizeof(*gi));
    guest_version(gi->agent_version, sizeof(gi->agent_version));
    struct utsname u;
    memset(&u, 0, sizeof(u));
    uname(&u);
    snprintf(gi->kernel_release, sizeof(gi->kernel_release), "%s",
             u.release[0] ? u.release : "unknown");
    os_release_get("NAME", gi->distro_name, sizeof(gi->distro_name));
    os_release_get("VERSION_ID", gi->distro_version, sizeof(gi->distro_version));
    os_release_get("ID", gi->os_id, sizeof(gi->os_id));
    os_release_get("ID_LIKE", gi->os_id_like, sizeof(gi->os_id_like));
    detect_init(gi->init_system, sizeof(gi->init_system));
    snprintf(gi->architecture, sizeof(gi->architecture), "%s",
             u.machine[0] ? u.machine : "unknown");
    struct np_environment_policy policy;
    current_environment_policy(&policy);
    snprintf(gi->environment_profile, sizeof(gi->environment_profile), "%s",
             policy.profile_id);
    gi->environment_revision = policy.revision;
    gi->have_wayland = np_path_exists("/run/nativepipe/wayland-0");
}

static int apply_guestd_from_fd(int fd, const np_agent_hdr *hdr) {
    if (np_agent_recv_payload_file(fd, hdr->payload_len, NP_INSTALLED_BIN, 0755) < 0)
        return -1;
    /* Version lives inside the new ELF; optional cache for operators. */
    if (hdr->version[0])
        np_write_version(hdr->version);
    return 0;
}

static void reexec(void) {
    char *args[] = {NP_INSTALLED_BIN, NULL};
    execv(NP_INSTALLED_BIN, args);
    _exit(127);
}

static int boot_self_update(void) {
    char ver[NP_MAX_VERSION];
    guest_version(ver, sizeof(ver));
    /* Migration/recovery check only. The normal live-update path is NPSY;
     * never delay the control listener for tens of seconds if the host file
     * service is temporarily unavailable. */
    int fd = np_vsock_connect_host(NP_PORT_AGENT, 3);
    if (fd < 0) {
        logmsg("self-update dial failed");
        return 0;
    }
    if (np_agent_send_request(fd, NP_GUESTD_NAME, ver) < 0) {
        close(fd);
        return 0;
    }
    np_agent_hdr hdr;
    if (np_agent_recv_hdr(fd, &hdr) < 0) {
        close(fd);
        return 0;
    }
    if (hdr.status == NP_STATUS_FILE || hdr.status == NP_STATUS_FORCE) {
        logmsg("updating guestd from host");
        if (apply_guestd_from_fd(fd, &hdr) == 0) {
            close(fd);
            reexec();
        }
        close(fd);
        return -1;
    }
    if (hdr.status == NP_STATUS_UPTODATE) {
        logmsg("agent up to date");
        close(fd);
        return 0;
    }
    close(fd);
    return 0;
}

static const char *first_executable(const char *const *paths) {
    for (size_t i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0)
            return paths[i];
    }
    return NULL;
}

static int run_grubby(void) {
    static const char *const paths[] = {
        "/usr/sbin/grubby", "/sbin/grubby", "/usr/bin/grubby", NULL,
    };
    const char *program = first_executable(paths);
    if (!program)
        return 127;
    char *argv[] = {
        (char *)program, "--update-kernel=ALL", "--args=" NP_CONSOLE_ARGUMENT, NULL,
    };
    return np_run(argv);
}

static int regenerate_grub(void) {
    static const char *const update_paths[] = {
        "/usr/sbin/update-grub", "/sbin/update-grub", "/usr/bin/update-grub", NULL,
    };
    const char *program = first_executable(update_paths);
    if (program) {
        char *argv[] = {(char *)program, NULL};
        return np_run(argv);
    }

    static const char *const mkconfig_paths[] = {
        "/usr/sbin/grub-mkconfig", "/sbin/grub-mkconfig",
        "/usr/bin/grub-mkconfig", "/usr/sbin/grub2-mkconfig",
        "/sbin/grub2-mkconfig", "/usr/bin/grub2-mkconfig", NULL,
    };
    program = first_executable(mkconfig_paths);
    if (!program)
        return 127;
    const char *output = NULL;
    if (np_path_exists("/boot/grub/grub.cfg") || np_path_exists("/boot/grub"))
        output = "/boot/grub/grub.cfg";
    else if (np_path_exists("/boot/grub2/grub.cfg") || np_path_exists("/boot/grub2"))
        output = "/boot/grub2/grub.cfg";
    if (!output)
        return 127;
    char *argv[] = {(char *)program, "-o", (char *)output, NULL};
    return np_run(argv);
}

static void start_console_getty(const char *init, unsigned changed,
                                unsigned char adapter) {
    if (adapter == NP_GETTY_NONE)
        return;
    if (adapter == NP_GETTY_AUTO) {
        if (is_systemd(init) && strcmp(init, "openrc") != 0)
            adapter = NP_GETTY_SYSTEMD;
        else if (is_openrc(init))
            adapter = NP_GETTY_INITTAB;
        else
            return;
    }
    if (adapter == NP_GETTY_SYSTEMD) {
        static const char *const paths[] = {
            "/usr/bin/systemctl", "/bin/systemctl", NULL,
        };
        const char *program = first_executable(paths);
        if (!program)
            return;
        char *argv[] = {
            (char *)program, "enable", "--now", "serial-getty@hvc0.service", NULL,
        };
        if (np_run(argv) != 0)
            logmsg("could not enable serial-getty@hvc0; kernel console remains configured");
        return;
    }
    if (adapter == NP_GETTY_INITTAB && (changed & NP_CONSOLE_METHOD_INITTAB)) {
        static const char *const paths[] = {
            "/sbin/telinit", "/usr/sbin/telinit", "/bin/telinit", NULL,
        };
        const char *program = first_executable(paths);
        if (program) {
            char *argv[] = {(char *)program, "q", NULL};
            np_run(argv);
        }
    }
}

/*
 * EFI cloud images boot their own kernel, so the host cannot inject its
 * linuxDirect baselineArguments.  guestd owns the persistent integration:
 * preserve distro arguments, add only console=hvc0, refresh the bootloader,
 * and make a login prompt available on the current hvc device when possible.
 */
static void console_integration_signature(char *out, size_t cap) {
    struct np_environment_policy policy;
    current_environment_policy(&policy);
    snprintf(out, cap, "%s:%llu:%s", NP_GUESTD_VERSION,
             (unsigned long long)policy.revision, policy.profile_id);
}

static int console_integration_is_current(void) {
    FILE *f = fopen(CONSOLE_INTEGRATION_STAMP, "r");
    if (!f)
        return 0;
    char version[NP_MAX_VERSION + NP_ENV_MAX_PROFILE_ID + 50];
    int ok = fgets(version, sizeof(version), f) != NULL;
    fclose(f);
    if (!ok)
        return 0;
    size_t n = strlen(version);
    while (n > 0 && (version[n - 1] == '\n' || version[n - 1] == '\r'))
        version[--n] = '\0';
    char expected[NP_MAX_VERSION + NP_ENV_MAX_PROFILE_ID + 48];
    console_integration_signature(expected, sizeof(expected));
    return strcmp(version, expected) == 0;
}

static void mark_console_integration_current(void) {
    char version[NP_MAX_VERSION + NP_ENV_MAX_PROFILE_ID + 50];
    console_integration_signature(version, sizeof(version));
    size_t n = strlen(version);
    if (n + 1 < sizeof(version)) {
        version[n++] = '\n';
        version[n] = '\0';
    }
    np_mkdir_p("/var/lib/nativepipe");
    if (np_write_file(CONSOLE_INTEGRATION_STAMP, version, n, 0644) < 0)
        logmsg("could not persist console integration stamp");
}

static int file_matches(const char *path, const char *expected, size_t expected_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char buf[512];
    if (expected_len > sizeof(buf)) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, sizeof(buf), f);
    int eof = feof(f);
    fclose(f);
    return eof && got == expected_len && memcmp(buf, expected, expected_len) == 0;
}

/*
 * cidata remains a recovery path, so disabling cloud-init would be wrong.
 * Restrict discovery to the local NoCloud disk and the immediate None fallback
 * instead; stock cloud images otherwise spend minutes probing EC2 metadata on
 * every boot after the one-shot seed has been detached.
 */
static void ensure_cloud_init_recovery_policy(void) {
    static const char path[] = "/etc/cloud/cloud.cfg.d/99_nativepipe_datasource.cfg";
    static const char policy[] =
        "# Managed by nativepipe-guestd; cidata is a recovery transport only.\n"
        "datasource_list: [ NoCloud, None ]\n";
    if (!np_path_exists("/etc/cloud/cloud.cfg.d"))
        return;
    if (file_matches(path, policy, sizeof(policy) - 1))
        return;
    if (np_write_file(path, policy, sizeof(policy) - 1, 0644) < 0)
        logmsg("could not configure local-only cloud-init datasource policy");
    else
        logmsg("configured cloud-init recovery datasource policy");
}

static void repair_cloud_init_cache(void) {
    int removed = np_cloud_init_remove_empty_network_cache("/");
    if (removed < 0)
        logmsg("could not repair empty cloud-init network cache");
    else if (removed > 0)
        fprintf(stderr, "[guestd] removed %d empty cloud-init network cache file(s)\n",
                removed);
}

static int ensure_console_integration(const char *init, int enable_getty) {
    struct np_environment_policy policy;
    current_environment_policy(&policy);
    int active = np_console_path_has_hvc0("/", "/proc/cmdline");
    struct np_console_result result;
    if (np_console_configure_files_selected(
            "/", policy.console_methods,
            (policy.flags & NP_ENV_DISABLE_UNAVAILABLE_GETTYS) != 0,
            &result) < 0) {
        fprintf(stderr, "[guestd] console provisioning failed: %s\n", strerror(errno));
        return -1;
    }

    const unsigned boot_methods =
        NP_CONSOLE_METHOD_GRUB | NP_CONSOLE_METHOD_LOADER_ENTRY |
        NP_CONSOLE_METHOD_KERNEL_CMDLINE | NP_CONSOLE_METHOD_EXTLINUX;
    if (!(result.detected & boot_methods)) {
        if (!active) {
            logmsg("no supported bootloader config found for console=hvc0");
            return -1;
        }
    } else if ((result.configured & boot_methods) != (result.detected & boot_methods)) {
        logmsg("some bootloader entries had no writable kernel options line");
    }

    int failed = 0;
    int need_next_boot = !active && (result.configured & boot_methods);
    if ((result.detected & NP_CONSOLE_METHOD_LOADER_ENTRY) &&
        (need_next_boot || (result.changed & NP_CONSOLE_METHOD_LOADER_ENTRY))) {
        int rc = run_grubby();
        if (rc != 0 && rc != 127) {
            fprintf(stderr, "[guestd] grubby console update failed: status %d\n", rc);
            failed = 1;
        }
    }
    if ((result.detected & NP_CONSOLE_METHOD_GRUB) &&
        (need_next_boot || (result.changed & NP_CONSOLE_METHOD_GRUB))) {
        int rc = regenerate_grub();
        if (rc == 127) {
            logmsg("GRUB config changed but no generator was found");
            failed = 1;
        } else if (rc != 0) {
            fprintf(stderr, "[guestd] GRUB regeneration failed: status %d\n", rc);
            failed = 1;
        }
    }

    if (enable_getty)
        start_console_getty(init, result.changed, policy.getty_adapter);
    if (result.changed)
        fprintf(stderr, "[guestd] console integration updated (methods=0x%x)\n",
                result.changed);
    if (need_next_boot)
        logmsg("console=hvc0 will carry kernel boot output after the next reboot");
    if (!failed && (active || (result.configured & boot_methods))) {
        mark_console_integration_current();
        return 0;
    }
    return -1;
}

static int apply_environment_policy(int enable_getty) {
    struct np_environment_policy policy;
    current_environment_policy(&policy);
    apply_cached_session_user_groups();
    if (policy.flags & NP_ENV_CLOUD_INIT_RECOVERY)
        ensure_cloud_init_recovery_policy();
    if (policy.flags & NP_ENV_REPAIR_CLOUD_INIT_NETWORK)
        repair_cloud_init_cache();
    char init[64];
    detect_init(init, sizeof(init));
    return ensure_console_integration(init, enable_getty);
}

/* Apply the exact profile selected by the host. The guest still validates the
 * profile's match constraints against live facts, and the catalog can select
 * only adapters compiled into this binary. */
static int reconcile_environment_profile(const char *profile_id,
                                         uint64_t desired_revision, int apply) {
    if (!profile_id || !profile_id[0] || desired_revision == 0) {
        errno = EINVAL;
        return -1;
    }
    struct np_environment_policy current;
    current_environment_policy(&current);
    if (current.revision == desired_revision &&
        strcmp(current.profile_id, profile_id) == 0)
        return apply ? apply_environment_policy(1) : 0;

    /* A profile can change without a revision change after importing an old
     * guest cache, so try the local catalog before transferring it again. */
    uint8_t *cached = NULL;
    size_t cached_len = 0;
    if (read_environment_catalog_file(NP_ENV_CATALOG_CACHE, &cached, &cached_len) == 0) {
        struct np_environment_policy selected;
        if (select_environment_catalog(cached, cached_len, profile_id, &selected) == 0 &&
            selected.revision == desired_revision) {
            int rc = install_environment_catalog(cached, cached_len, profile_id, 1);
            free(cached);
            if (rc < 0)
                return -1;
            return apply ? apply_environment_policy(1) : 0;
        }
        free(cached);
    }

    uint8_t *data = NULL;
    size_t len = 0;
    char host_revision[NP_MAX_VERSION];
    int rc = np_agent_pull_mem_n(NP_ENV_CATALOG_NAME, "", &data, &len,
                                 host_revision, sizeof(host_revision), 3);
    if (rc != 0 || !data) {
        free(data);
        return -1;
    }
    char expected[32];
    snprintf(expected, sizeof(expected), "%llu", (unsigned long long)desired_revision);
    if (strcmp(host_revision, expected) != 0) {
        free(data);
        errno = ESTALE;
        return -1;
    }
    rc = install_environment_catalog(data, len, profile_id, 1);
    free(data);
    if (rc < 0) {
        fprintf(stderr, "[guestd] rejected environment catalog: %s\n", strerror(errno));
        return -1;
    }
    if (apply)
        return apply_environment_policy(1);
    return 0;
}

/* Compatibility for the old NPEU request: update the already host-selected
 * profile only. It never performs a new match inside the guest. */
static int refresh_current_environment_profile(int apply) {
    struct np_environment_policy current;
    current_environment_policy(&current);
    if (!current.revision || !current.profile_id[0]) {
        errno = ENOENT;
        return -1;
    }
    uint8_t *data = NULL;
    size_t len = 0;
    char revision[32];
    snprintf(revision, sizeof(revision), "%llu",
             (unsigned long long)current.revision);
    int rc = np_agent_pull_mem_n(NP_ENV_CATALOG_NAME, revision, &data, &len,
                                 NULL, 0, 3);
    if (rc == 1)
        return 0;
    if (rc != 0 || !data) {
        free(data);
        return -1;
    }
    struct np_environment_policy selected;
    if (select_environment_catalog(data, len, current.profile_id, &selected) < 0) {
        free(data);
        return -1;
    }
    rc = install_environment_catalog(data, len, current.profile_id, 1);
    free(data);
    if (rc < 0)
        return -1;
    return apply ? apply_environment_policy(1) : 0;
}

static unsigned char resolved_service_adapter(const char *init) {
    struct np_environment_policy policy;
    current_environment_policy(&policy);
    if (policy.service_adapter != NP_SERVICE_AUTO)
        return policy.service_adapter;
    if (is_systemd(init) && strcmp(init, "openrc") != 0)
        return NP_SERVICE_SYSTEMD;
    if (is_openrc(init))
        return NP_SERVICE_OPENRC;
    return NP_SERVICE_PROCESS;
}

static int provision(void) {
    if (geteuid() != 0) {
        logmsg("provision must run as root");
        return 1;
    }
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        if (strcmp(self, NP_INSTALLED_BIN) != 0) {
            np_mkdir_p("/usr/libexec/nativepipe");
            np_copy_file(self, NP_INSTALLED_BIN, 0755);
        }
    }
    char ver[NP_MAX_VERSION];
    guest_version(ver, sizeof(ver));
    np_write_version(ver); /* cache for operators; not used for identity */

    char init[64];
    detect_init(init, sizeof(init));
    fprintf(stderr, "[guestd] provisioning %s for init=%s\n", ver, init);
    load_cached_environment_catalog();
    /* Bootstrap must be distro-neutral. Host matching happens after guestd's
     * control listener is reachable; until then use only compiled discovery. */
    if (has_host_environment_policy())
        apply_environment_policy(1);

    unsigned char service_adapter = resolved_service_adapter(init);
    if (service_adapter == NP_SERVICE_SYSTEMD) {
        uint8_t *mem = NULL;
        size_t len = 0;
        int rc = np_agent_pull_mem("systemd/nativepipe-guestd.service", "", &mem, &len,
                                   NULL, 0);
        if (rc != 0 || !mem) {
            logmsg("host has no systemd unit");
            return 1;
        }
        np_write_file("/etc/systemd/system/nativepipe-guestd.service", mem, len, 0644);
        free(mem);
        char *reload[] = {"systemctl", "daemon-reload", NULL};
        char *enable[] = {"systemctl", "enable", "nativepipe-guestd.service", NULL};
        char *start[] = {"systemctl", "start", "nativepipe-guestd.service", NULL};
        np_run(reload);
        np_run(enable);
        np_run(start);
        return 0;
    }
    if (service_adapter == NP_SERVICE_OPENRC) {
        uint8_t *mem = NULL;
        size_t len = 0;
        int rc = np_agent_pull_mem("openrc/nativepipe-guestd", "", &mem, &len, NULL, 0);
        if (rc != 0 || !mem) {
            logmsg("host has no OpenRC script");
            return 1;
        }
        np_write_file("/etc/init.d/nativepipe-guestd", mem, len, 0755);
        free(mem);
        char *add[] = {"rc-update", "add", "nativepipe-guestd", "default", NULL};
        char *start[] = {"rc-service", "nativepipe-guestd", "start", NULL};
        np_run(add);
        np_run(start);
        return 0;
    }
    logmsg("environment profile selected process supervision fallback");
    pid_t pid = fork();
    if (pid == 0) {
        reexec();
    }
    return 0;
}

static int valid_username(const char *user) {
    if (!user || !user[0] || strlen(user) > 32)
        return 0;
    unsigned char c = (unsigned char)user[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
        return 0;
    for (const char *p = user + 1; *p; p++) {
        c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

static int shell_is_listed(const char *path) {
    FILE *f = fopen("/etc/shells", "r");
    if (!f)
        return strcmp(path, "/bin/sh") == 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *start = line;
        while (*start == ' ' || *start == '\t')
            start++;
        char *end = start + strlen(start);
        while (end > start && (end[-1] == '\n' || end[-1] == '\r' ||
                               end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*start != '#' && strcmp(start, path) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static const char *preferred_login_shell(void) {
    static _Thread_local char selected[NP_ENV_MAX_SHELL_PATH];
    struct np_environment_policy policy;
    current_environment_policy(&policy);
    for (size_t i = 0; i < policy.shell_count; i++) {
        const char *candidate = policy.login_shells[i];
        if (access(candidate, X_OK) == 0 && shell_is_listed(candidate)) {
            snprintf(selected, sizeof(selected), "%s", candidate);
            return selected;
        }
    }
    /* /etc/shells is optional in tiny images; POSIX /bin/sh remains the only
     * hard safety fallback and is never supplied by the network catalog. */
    if (access("/bin/sh", X_OK) == 0) {
        snprintf(selected, sizeof(selected), "/bin/sh");
        return selected;
    }
    return NULL;
}

/* Preserve a valid user choice, but repair cloud users that were created with
 * a distro-specific shell which is absent in the current root filesystem. */
static int ensure_login_shell(const char *user) {
    struct passwd *pw = getpwnam(user);
    if (!pw) {
        errno = ENOENT;
        return -1;
    }
    if (pw->pw_shell && pw->pw_shell[0] && access(pw->pw_shell, X_OK) == 0)
        return 0;

    const char *shell = preferred_login_shell();
    if (!shell) {
        errno = ENOENT;
        return -1;
    }
    char *modify[] = {"usermod", "-s", (char *)shell, (char *)user, NULL};
    if (np_run(modify) != 0) {
        char *change[] = {"chsh", "-s", (char *)shell, (char *)user, NULL};
        if (np_run(change) != 0)
            return -1;
    }
    fprintf(stderr, "[guestd] repaired login shell for %s: %s\n", user, shell);
    return 0;
}

static int ensure_user(const char *user) {
    if (strcmp(user, "root") == 0)
        return 0;
    if (getpwnam(user))
        return ensure_login_shell(user);

    const char *shell = preferred_login_shell();
    if (!shell) {
        errno = ENOENT;
        return -1;
    }
    char *ua[] = {"useradd", "-m", "-s", (char *)shell, (char *)user, NULL};
    if (np_run(ua) != 0) {
        char *au[] = {"adduser", "-D", "-s", (char *)shell, (char *)user, NULL};
        if (np_run(au) != 0)
            return -1;
    }
    return ensure_login_shell(user);
}

static int group_contains_user(const struct group *group, const char *user,
                               gid_t primary_gid) {
    if (primary_gid == group->gr_gid)
        return 1;
    if (!group->gr_mem)
        return 0;
    for (char **member = group->gr_mem; *member; member++) {
        if (strcmp(*member, user) == 0)
            return 1;
    }
    return 0;
}

/* Add only to a group that already exists in the guest. The group name comes
 * from a compiled bit-to-adapter mapping below, never directly from catalog
 * bytes. usermod is the portable primary path; addgroup covers BusyBox images. */
static int add_user_to_existing_group(const char *user, const char *group_name) {
    struct passwd *pw = getpwnam(user);
    if (!pw) {
        errno = ENOENT;
        return -1;
    }
    /* Copy the only passwd field used below before the next NSS lookup. */
    gid_t primary_gid = pw->pw_gid;
    struct group *group = getgrnam(group_name);
    if (!group)
        return 0;
    if (group_contains_user(group, user, primary_gid))
        return 0;

    char *modify[] = {"usermod", "-aG", (char *)group_name, (char *)user, NULL};
    if (np_run(modify) != 0) {
        char *add[] = {"addgroup", (char *)user, (char *)group_name, NULL};
        if (np_run(add) != 0)
            return -1;
    }
    fprintf(stderr, "[guestd] added %s to %s\n", user, group_name);
    return 1;
}

/* Supplementary groups are captured by initgroups() when nativepipe-session
 * drops privileges. If a graphical session is already running, merely editing
 * /etc/group leaves PipeWire and the compositor with the old credentials. */
static void restart_running_graphical_session(void) {
    char init[64];
    detect_init(init, sizeof(init));
    if (strcmp(init, "systemd") == 0) {
        char *active[] = {"systemctl", "is-active", "--quiet",
                          "nativepipe-session.service", NULL};
        if (np_run(active) == 0) {
            char *restart[] = {"systemctl", "restart",
                               "nativepipe-session.service", NULL};
            if (np_run(restart) != 0)
                logmsg("could not restart graphical session after group update");
        }
        return;
    }
    if (strcmp(init, "openrc") == 0) {
        char *status[] = {"rc-service", "nativepipe-session", "status", NULL};
        if (np_run(status) == 0) {
            char *restart[] = {"rc-service", "nativepipe-session", "restart", NULL};
            if (np_run(restart) != 0)
                logmsg("could not restart graphical session after group update");
        }
    }
}

static void apply_session_groups(const char *user) {
    if (!user || strcmp(user, "root") == 0)
        return;

    /* setUser and a live catalog update are independent asynchronous RPCs.
     * Serialize /etc/group writers so their idempotent additions cannot race. */
    pthread_mutex_lock(&session_group_lock);
    int runtime_groups_changed = 0;
    runtime_groups_changed |= add_user_to_existing_group(user, "audio") > 0;
    runtime_groups_changed |= add_user_to_existing_group(user, "input") > 0;
    runtime_groups_changed |= add_user_to_existing_group(user, "render") > 0;
    runtime_groups_changed |= add_user_to_existing_group(user, "video") > 0;

    struct np_environment_policy policy;
    current_environment_policy(&policy);
    if (policy.administrator_groups & NP_ADMINISTRATOR_GROUP_WHEEL)
        add_user_to_existing_group(user, "wheel");
    if (policy.administrator_groups & NP_ADMINISTRATOR_GROUP_SUDO)
        add_user_to_existing_group(user, "sudo");
    pthread_mutex_unlock(&session_group_lock);
    if (runtime_groups_changed)
        restart_running_graphical_session();
}

static int read_cached_session_user(char *user, size_t cap) {
    FILE *file = fopen(NP_SESSION_USER_FILE, "r");
    if (!file)
        return -1;
    if (!fgets(user, (int)cap, file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    size_t len = strlen(user);
    while (len > 0 && (user[len - 1] == '\n' || user[len - 1] == '\r' ||
                       user[len - 1] == ' ' || user[len - 1] == '\t'))
        user[--len] = '\0';
    return valid_username(user) ? 0 : -1;
}

static void apply_cached_session_user_groups(void) {
    char user[64];
    if (read_cached_session_user(user, sizeof(user)) == 0)
        apply_session_groups(user);
}

enum { NP_SESSION_STACK_ARTIFACT_COUNT = 8 };

struct session_stack_install_context {
    char staging[NP_SESSION_STACK_ARTIFACT_COUNT][768];
    char versions[NP_SESSION_STACK_ARTIFACT_COUNT][NP_MAX_VERSION];
    struct np_session_stack_file_state file_state[NP_SESSION_STACK_ARTIFACT_COUNT];
};

static int stage_session_artifact(
    void *opaque, const struct np_session_stack_artifact *artifact,
    size_t index) {
    struct session_stack_install_context *context = opaque;
    if (index >= NP_SESSION_STACK_ARTIFACT_COUNT) {
        errno = EOVERFLOW;
        return -1;
    }
    if (snprintf(context->staging[index], sizeof(context->staging[index]),
                 "%s.nativepipe-stage.%ld.%zu", artifact->destination,
                 (long)getpid(), index) >= (int)sizeof(context->staging[index])) {
        errno = ENAMETOOLONG;
        return -1;
    }
    unlink(context->staging[index]);
    context->versions[index][0] = '\0';
    int rc = np_agent_pull_file_mode_n(
        artifact->name, "", context->staging[index], artifact->mode,
        context->versions[index], sizeof(context->versions[index]), 8);
    if (rc != 0) {
        unlink(context->staging[index]);
        context->staging[index][0] = '\0';
    }
    return rc;
}

static int commit_session_artifact(
    void *opaque, const struct np_session_stack_artifact *artifact,
    size_t index) {
    struct session_stack_install_context *context = opaque;
    if (index >= NP_SESSION_STACK_ARTIFACT_COUNT ||
        context->staging[index][0] == '\0' ||
        context->file_state[index].committed) {
        errno = EINVAL;
        return -1;
    }
    char backup_path[NP_SESSION_STACK_PATH_CAP];
    if (snprintf(backup_path, sizeof(backup_path),
                 "%s.nativepipe-backup.%ld.%zu", artifact->destination,
                 (long)getpid(), index) >= (int)sizeof(backup_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (np_session_stack_commit_file(
            artifact, context->staging[index], backup_path,
            &context->file_state[index]) < 0)
        return -1;
    context->staging[index][0] = '\0';
    fprintf(stderr, "[guestd] installed %s %s\n", artifact->name,
            context->versions[index][0] ? context->versions[index] : "");
    return 0;
}

static int rollback_session_artifact(
    void *opaque, const struct np_session_stack_artifact *artifact,
    size_t index) {
    struct session_stack_install_context *context = opaque;
    if (index >= NP_SESSION_STACK_ARTIFACT_COUNT)
        return -1;
    return np_session_stack_rollback_file(
        artifact, &context->file_state[index]);
}

static void finish_session_artifact(
    void *opaque, const struct np_session_stack_artifact *artifact,
    size_t index) {
    (void)artifact;
    struct session_stack_install_context *context = opaque;
    if (index >= NP_SESSION_STACK_ARTIFACT_COUNT)
        return;
    if (np_session_stack_finish_file(&context->file_state[index]) < 0)
        logmsg("could not remove obsolete session artifact backup");
}

static void discard_session_artifact(
    void *opaque, const struct np_session_stack_artifact *artifact,
    size_t index) {
    (void)artifact;
    struct session_stack_install_context *context = opaque;
    if (index < NP_SESSION_STACK_ARTIFACT_COUNT &&
        context->staging[index][0] != '\0') {
        unlink(context->staging[index]);
        context->staging[index][0] = '\0';
    }
}

static int run_session_stack_command(void *context,
                                     const char *const arguments[]) {
    (void)context;
    return np_run((char *const *)arguments);
}

/* The agent itself is static, so inspect the guest rootfs rather than its own libc. */
static int rootfs_uses_musl(void) {
    DIR *dir = opendir("/lib");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "ld-musl-", 8) == 0 &&
                strstr(entry->d_name, ".so") != NULL) {
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }
    return 0;
}

static const char *libc_artifact_name(const char *musl, const char *gnu) {
    return rootfs_uses_musl() ? musl : gnu;
}

/* Pull a complete display generation before restarting the empty greeter. */
static void ensure_session_stack(void) {
    pthread_mutex_lock(&session_stack_lock);
    if (session_stack_ready) {
        pthread_mutex_unlock(&session_stack_lock);
        return;
    }
    char init[64];
    detect_init(init, sizeof(init));
    unsigned char service_adapter = resolved_service_adapter(init);
    const char *unit_name;
    const char *unit_destination;
    int unit_mode;
    enum np_session_stack_service service;
    if (service_adapter == NP_SERVICE_SYSTEMD) {
        unit_name = "systemd/nativepipe-session.service";
        unit_destination = "/etc/systemd/system/nativepipe-session.service";
        unit_mode = 0644;
        service = NP_SESSION_STACK_SYSTEMD;
    } else if (service_adapter == NP_SERVICE_OPENRC) {
        unit_name = "openrc/nativepipe-session";
        unit_destination = "/etc/init.d/nativepipe-session";
        unit_mode = 0755;
        service = NP_SESSION_STACK_OPENRC;
    } else {
        logmsg("cannot install graphical session without systemd or OpenRC");
        pthread_mutex_unlock(&session_stack_lock);
        return;
    }
    if (np_mkdir_p("/usr/local/bin") < 0 ||
        np_mkdir_p("/etc/vulkan/implicit_layer.d") < 0 ||
        np_mkdir_p("/etc/profile.d") < 0) {
        logmsg("cannot create graphical session installation directories");
        pthread_mutex_unlock(&session_stack_lock);
        return;
    }

    const struct np_session_stack_artifact artifacts[] = {
        {NP_SESSION_NAME, NP_INSTALLED_SESSION, 0755},
        {libc_artifact_name(NP_COMPOSITOR_MUSL_NAME, NP_COMPOSITOR_GNU_NAME),
         NP_INSTALLED_COMPOSITOR, 0755},
        {libc_artifact_name(NP_XWAYLAND_SATELLITE_MUSL_NAME,
                            NP_XWAYLAND_SATELLITE_GNU_NAME),
         NP_INSTALLED_XWAYLAND_SATELLITE, 0755},
        {libc_artifact_name(NP_ALIGN_BLOB_MUSL_NAME, NP_ALIGN_BLOB_GNU_NAME),
         NP_INSTALLED_ALIGN_BLOB, 0755},
        {libc_artifact_name(NP_VULKAN_LAYER_MUSL_NAME, NP_VULKAN_LAYER_GNU_NAME),
         NP_INSTALLED_VULKAN_LAYER, 0755},
        {NP_VULKAN_LAYER_MANIFEST_NAME, NP_INSTALLED_VULKAN_LAYER_MANIFEST, 0644},
        {NP_SESSION_PROFILE_NAME, NP_INSTALLED_SESSION_PROFILE, 0644},
        {unit_name, unit_destination, unit_mode},
    };
    struct session_stack_install_context install_context;
    memset(&install_context, 0, sizeof(install_context));
    const struct np_session_stack_operations operations = {
        .stage = stage_session_artifact,
        .commit = commit_session_artifact,
        .rollback = rollback_session_artifact,
        .finish = finish_session_artifact,
        .discard = discard_session_artifact,
        .run = run_session_stack_command,
        .context = &install_context,
    };
    if (np_session_stack_apply(
            artifacts, sizeof(artifacts) / sizeof(artifacts[0]),
            service, &operations) == 0) {
        session_stack_ready = 1;
    } else {
        logmsg("graphical session stack is incomplete; keeping the current service state");
    }
    pthread_mutex_unlock(&session_stack_lock);
}

static void prepare_graphical_session(const char *user) {
    if (!user || !user[0] || strcmp(user, "root") == 0)
        return;
    /* Publish the selected user before applying groups. If a catalog update
     * overlaps this request, either side will then observe enough state to
     * apply the newest administrator policy. */
    np_mkdir_p("/var/lib/nativepipe");
    if (np_write_file(NP_SESSION_USER_FILE, user, strlen(user), 0644) < 0)
        logmsg("could not persist graphical session user");
    apply_session_groups(user);
    char *linger[] = {"loginctl", "enable-linger", (char *)user, NULL};
    np_run(linger);
    ensure_session_stack();
}

static void *session_stack_thread(void *arg) {
    (void)arg;
    ensure_session_stack();
    return NULL;
}

static int run_chpasswd(const char *user, const char *password) {
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[1]);
        if (dup2(fds[0], 0) < 0)
            _exit(127);
        close(fds[0]);
        execlp("chpasswd", "chpasswd", (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    char line[512];
    int n = snprintf(line, sizeof(line), "%s:%s\n", user, password);
    int st = 0;
    if (n < 0 || n >= (int)sizeof(line) || np_write_full(fds[1], line, (size_t)n) < 0) {
        close(fds[1]);
        waitpid(pid, &st, 0);
        return -1;
    }
    close(fds[1]);
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return -1;
}

static int set_user(const char *user, const char *old_user) {
    if (!valid_username(user))
        return -1;
    if (old_user && old_user[0]) {
        if (!valid_username(old_user))
            return -1;
        if (getpwnam(user)) {
            errno = EEXIST;
            return -1;
        }
        if (!getpwnam(old_user)) {
            errno = ENOENT;
            return -1;
        }
        char *ren[] = {"usermod", "-l", (char *)user, (char *)old_user, NULL};
        if (np_run(ren) != 0)
            return -1;
        char home[256];
        snprintf(home, sizeof(home), "/home/%s", user);
        char *mv[] = {"usermod", "-d", home, "-m", (char *)user, NULL};
        np_run(mv);
        char *gr[] = {"groupmod", "-n", (char *)user, (char *)old_user, NULL};
        np_run(gr);
        return ensure_login_shell(user);
    }
    return ensure_user(user);
}

static int set_password(const char *user, const char *password) {
    if (!valid_username(user) || !password || strchr(password, '\n'))
        return -1;
    if (!getpwnam(user) && strcmp(user, "root") != 0) {
        errno = ENOENT;
        return -1;
    }
    return run_chpasswd(user, password) == 0 ? 0 : -1;
}

static int resize_console(int cols, int rows) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    int fd = open(CONSOLE_TTY, O_RDWR | O_NOCTTY);
    if (fd < 0)
        return -1;
    int rc = ioctl(fd, TIOCSWINSZ, &ws);
    close(fd);
    return rc;
}

static void apply_env(const struct launch_req *r) {
    for (int i = 0; i < r->nenv; i++)
        setenv(r->env_keys[i], r->env_vals[i], 1);
}

struct session_environment {
    char display[128];
    char xdisplay[64];
    char xauthority[256];
    char dbus[1536];
};

static int valid_display_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')
            continue;
        return 0;
    }
    return 1;
}

static int valid_x_display_name(const char *name) {
    if (!name || name[0] != ':' || !name[1]) return 0;
    int after_dot = 0;
    int display_digits = 0;
    int screen_digits = 0;
    for (const unsigned char *p = (const unsigned char *)name + 1; *p; p++) {
        if (*p == '.' && !after_dot && display_digits) { after_dot = 1; continue; }
        if (*p < '0' || *p > '9') return 0;
        if (after_dot) screen_digits++;
        else display_digits++;
    }
    return display_digits > 0 && (!after_dot || screen_digits > 0);
}

/* nativepipe-wayland.env is a readiness record written atomically by the
 * compositor.  Verify both its owner and the named socket before importing
 * anything; a stale file must not redirect a newly launched desktop client. */
static int read_session_environment(uid_t uid, struct session_environment *out) {
    char path[160];
    snprintf(path, sizeof(path), "/run/user/%u/nativepipe-wayland.env", (unsigned)uid);
    struct stat env_stat;
    if (stat(path, &env_stat) < 0 || !S_ISREG(env_stat.st_mode) || env_stat.st_uid != uid)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    memset(out, 0, sizeof(*out));
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (strcmp(line, "WAYLAND_DISPLAY") == 0) {
            if (strlen(eq) >= sizeof(out->display)) {
                fclose(f);
                return -1;
            }
            strcpy(out->display, eq);
        } else if (strcmp(line, "DISPLAY") == 0) {
            if (strlen(eq) >= sizeof(out->xdisplay)) {
                fclose(f);
                return -1;
            }
            strcpy(out->xdisplay, eq);
        } else if (strcmp(line, "XAUTHORITY") == 0) {
            if (strlen(eq) >= sizeof(out->xauthority)) {
                fclose(f);
                return -1;
            }
            strcpy(out->xauthority, eq);
        } else if (strcmp(line, "DBUS_SESSION_BUS_ADDRESS") == 0) {
            if (strlen(eq) >= sizeof(out->dbus)) {
                fclose(f);
                return -1;
            }
            strcpy(out->dbus, eq);
        }
    }
    fclose(f);

    if (!valid_display_name(out->display)) return -1;
    if (out->xdisplay[0] && !valid_x_display_name(out->xdisplay)) return -1;
    char socket_path[320];
    snprintf(socket_path, sizeof(socket_path), "/run/user/%u/%s",
             (unsigned)uid, out->display);
    struct stat socket_stat;
    if (lstat(socket_path, &socket_stat) < 0 || !S_ISSOCK(socket_stat.st_mode) ||
        socket_stat.st_uid != uid)
        return -1;
    if (out->xdisplay[0]) {
        char runtime_prefix[96];
        snprintf(runtime_prefix, sizeof(runtime_prefix), "/run/user/%u/",
                 (unsigned)uid);
        size_t prefix_len = strlen(runtime_prefix);
        struct stat auth_stat;
        if (!out->xauthority[0] ||
            strncmp(out->xauthority, runtime_prefix, prefix_len) != 0 ||
            strchr(out->xauthority + prefix_len, '/') ||
            lstat(out->xauthority, &auth_stat) < 0 ||
            !S_ISREG(auth_stat.st_mode) || auth_stat.st_uid != uid ||
            (auth_stat.st_mode & 077) != 0)
            return -1;
    } else if (out->xauthority[0]) {
        return -1;
    }
    return 0;
}

static int import_session_environment(uid_t uid) {
    struct session_environment env;
    if (read_session_environment(uid, &env) < 0) return -1;
    if (setenv("WAYLAND_DISPLAY", env.display, 1) < 0) return -1;
    if (env.xdisplay[0] && setenv("DISPLAY", env.xdisplay, 1) < 0)
        return -1;
    if (env.xauthority[0] && setenv("XAUTHORITY", env.xauthority, 1) < 0)
        return -1;
    if (env.dbus[0] && setenv("DBUS_SESSION_BUS_ADDRESS", env.dbus, 1) < 0)
        return -1;
    return 0;
}

static const char *gsettings_program(void) {
    static const char *paths[] = {"/usr/bin/gsettings", "/bin/gsettings", NULL};
    for (size_t i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0)
            return paths[i];
    }
    return NULL;
}

/* Apply to a live desktop when one exists. The persisted preference remains
 * authoritative when gsettings or the user bus is absent; nativepipe-session
 * retries it when the next graphical session starts. */
static void apply_desktop_preferences_to_session(uint8_t color_scheme) {
    const char *program = gsettings_program();
    char user[64];
    if (!program || read_cached_session_user(user, sizeof(user)) < 0)
        return;
    struct passwd *pw = getpwnam(user);
    struct session_environment env;
    if (!pw || pw->pw_uid == 0 || read_session_environment(pw->pw_uid, &env) < 0 ||
        !env.dbus[0])
        return;

    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0) {
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        return;
    }

    char runtime[96];
    snprintf(runtime, sizeof(runtime), "/run/user/%u", (unsigned)pw->pw_uid);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("HOME", pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : "/", 1);
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", env.dbus, 1);
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 ||
        setuid(pw->pw_uid) < 0)
        _exit(126);
    const char *value = color_scheme == 2 ? "prefer-dark" : "prefer-light";
    char *argv[] = {(char *)program, "set", "org.gnome.desktop.interface",
                    "color-scheme", (char *)value, NULL};
    execv(program, argv);
    _exit(127);
}

static int enter_launch_context(const struct launch_req *r) {
    if (!r->username || !r->username[0]) {
        if (r->have_cwd && chdir(r->cwd) < 0)
            return -1;
        apply_env(r);
        return 0;
    }

    struct passwd *pw = getpwnam(r->username);
    if (!pw || pw->pw_uid == 0)
        return -1;

    char runtime[96];
    snprintf(runtime, sizeof(runtime), "/run/user/%u", (unsigned)pw->pw_uid);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("HOME", pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : "/", 1);
    setenv("SHELL", pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/sh", 1);
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "NativePipe", 1);
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("DISPLAY");
    unsetenv("XAUTHORITY");
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    int has_venus = np_venus_icd_available();
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0 && page_size < 16384) {
        if (access(NP_INSTALLED_ALIGN_BLOB, R_OK) == 0)
            setenv("LD_PRELOAD", NP_INSTALLED_ALIGN_BLOB, 0);
        if (has_venus && access(NP_INSTALLED_VULKAN_LAYER, R_OK) == 0 &&
            access(NP_INSTALLED_VULKAN_LAYER_MANIFEST, R_OK) == 0)
            setenv("NATIVEPIPE_BLOB_ALIGNMENT", "16384", 0);
    }
    (void)import_session_environment(pw->pw_uid);
    apply_env(r);

    const char *cwd = r->have_cwd ? r->cwd : pw->pw_dir;
    if (cwd && cwd[0] && chdir(cwd) < 0)
        return -1;
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 ||
        setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0)
        return -1;
    return 0;
}

struct launch_reaper {
    pid_t pid;
    int control_fd;
};

static int send_process_exited(int fd, int32_t pid, int32_t status);

static void *reap_child_thread(void *arg) {
    struct launch_reaper *reaper = arg;
    int wait_status = 0;
    pid_t waited;
    do { waited = waitpid(reaper->pid, &wait_status, 0); }
    while (waited < 0 && errno == EINTR);
    int status = waited == reaper->pid && WIFEXITED(wait_status)
        ? WEXITSTATUS(wait_status)
        : waited == reaper->pid && WIFSIGNALED(wait_status)
            ? 128 + WTERMSIG(wait_status) : 1;
    (void)send_process_exited(reaper->control_fd, reaper->pid, status);
    close(reaper->control_fd);
    free(reaper);
    return NULL;
}

static void reap_child_async(pid_t pid, int control_fd) {
    struct launch_reaper *reaper = calloc(1, sizeof(*reaper));
    if (!reaper) return;
    reaper->pid = pid;
    reaper->control_fd = fcntl(control_fd, F_DUPFD_CLOEXEC, 0);
    if (reaper->control_fd < 0) {
        free(reaper);
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, reap_child_thread, reaper) == 0)
        pthread_detach(thread);
    else {
        close(reaper->control_fd);
        free(reaper);
    }
}

static int launch_spec(const struct launch_req *r, int *pid_out) {
    if (!r->exe || !r->exe[0])
        return -1;
    if (r->username && r->username[0]) {
        struct passwd *pw = getpwnam(r->username);
        struct session_environment env;
        if (!pw || pw->pw_uid == 0 || read_session_environment(pw->pw_uid, &env) < 0) {
            errno = EAGAIN;
            return -1;
        }
    }
    int status_pipe[2];
    if (pipe2(status_pipe, O_CLOEXEC) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
    {
        int saved = errno;
        close(status_pipe[0]);
        close(status_pipe[1]);
        errno = saved;
        return -1;
    }
    if (pid == 0) {
        close(status_pipe[0]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            close(devnull);
        }
        setsid();
        if (enter_launch_context(r) < 0) {
            int failure = errno ? errno : EIO;
            (void)write(status_pipe[1], &failure, sizeof(failure));
            _exit(126);
        }
        char *argv[NP_MAX_ARGS + 2];
        argv[0] = (char *)r->exe;
        for (int i = 0; i < r->nargs; i++)
            argv[i + 1] = (char *)r->args[i];
        argv[r->nargs + 1] = NULL;
        execvp(r->exe, argv);
        int failure = errno ? errno : ENOENT;
        (void)write(status_pipe[1], &failure, sizeof(failure));
        _exit(127);
    }
    close(status_pipe[1]);
    int failure = 0;
    size_t received = 0;
    while (received < sizeof(failure)) {
        ssize_t n = read(status_pipe[0], (char *)&failure + received,
                         sizeof(failure) - received);
        if (n > 0) {
            received += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && received == 0)
            failure = errno;
        break;
    }
    close(status_pipe[0]);
    if (received != 0 || failure != 0) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        errno = failure ? failure : EIO;
        return -1;
    }
    *pid_out = (int)pid;
    return 0;
}

static int read_pipe_pair(int out_fd, int err_fd,
                          uint8_t **out, size_t *out_len, size_t out_cap,
                          uint8_t **err, size_t *err_len, size_t err_cap) {
    uint8_t *out_buf = malloc(out_cap + 1);
    uint8_t *err_buf = malloc(err_cap + 1);
    if (!out_buf || !err_buf) {
        free(out_buf);
        free(err_buf);
        return -1;
    }
    size_t lengths[2] = {0, 0};
    size_t caps[2] = {out_cap, err_cap};
    uint8_t *buffers[2] = {out_buf, err_buf};
    struct pollfd fds[2] = {
        {.fd = out_fd, .events = POLLIN | POLLHUP},
        {.fd = err_fd, .events = POLLIN | POLLHUP},
    };
    int live = 2;
    uint8_t discard[8192];
    while (live > 0) {
        int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            free(out_buf); free(err_buf);
            return -1;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            size_t remaining = caps[i] - lengths[i];
            void *target = remaining ? buffers[i] + lengths[i] : discard;
            size_t amount = remaining ? remaining : sizeof(discard);
            ssize_t n = read(fds[i].fd, target, amount);
            if (n > 0) {
                if (remaining) lengths[i] += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            close(fds[i].fd);
            fds[i].fd = -1;
            live--;
        }
    }
    out_buf[lengths[0]] = 0;
    err_buf[lengths[1]] = 0;
    *out = out_buf; *out_len = lengths[0];
    *err = err_buf; *err_len = lengths[1];
    return 0;
}

static int run_spec(const struct launch_req *r, int *status_out, uint8_t **stdout_b,
                    size_t *stdout_n, uint8_t **stderr_b, size_t *stderr_n) {
    if (!r->exe || !r->exe[0])
        return -1;
    int in_pipe[2] = {-1, -1}, out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0)
        return -1;
    if (r->stdin_len && pipe(in_pipe) < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        if (in_pipe[0] >= 0) {
            close(in_pipe[0]);
            close(in_pipe[1]);
        }
        return -1;
    }
    if (pid == 0) {
        if (r->stdin_len) {
            close(in_pipe[1]);
            dup2(in_pipe[0], 0);
            close(in_pipe[0]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, 0);
                close(devnull);
            }
        }
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], 1);
        dup2(err_pipe[1], 2);
        close(out_pipe[1]);
        close(err_pipe[1]);
        if (enter_launch_context(r) < 0)
            _exit(126);
        char *argv[NP_MAX_ARGS + 2];
        argv[0] = (char *)r->exe;
        for (int i = 0; i < r->nargs; i++)
            argv[i + 1] = (char *)r->args[i];
        argv[r->nargs + 1] = NULL;
        execvp(r->exe, argv);
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (r->stdin_len) {
        close(in_pipe[0]);
        np_write_full(in_pipe[1], r->stdin_data, r->stdin_len);
        close(in_pipe[1]);
    }

    uint8_t *outb = NULL, *errb = NULL;
    size_t outn = 0, errn = 0;
    if (read_pipe_pair(out_pipe[0], err_pipe[0], &outb, &outn, NP_MAX_READ,
                       &errb, &errn, NP_MAX_STDERR) < 0) {
        kill(pid, SIGTERM);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    *status_out = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    *stdout_b = outb ? outb : (uint8_t *)calloc(1, 1);
    *stdout_n = outb ? outn : 0;
    *stderr_b = errb ? errb : (uint8_t *)calloc(1, 1);
    *stderr_n = errb ? errn : 0;
    return 0;
}

struct exec_session {
    int listen_fd;
    int master_fd;
    pid_t pid;
    uint8_t input[64 * 1024];
    size_t input_len;
};

static void apply_pty_winsize(int master_fd, uint32_t cols, uint32_t rows) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = cols ? (unsigned short)cols : 80;
    ws.ws_row = rows ? (unsigned short)rows : 24;
    if (ioctl(master_fd, TIOCSWINSZ, &ws) < 0)
        logmsg("TIOCSWINSZ failed");
}

#define NP_EXEC_HEADER 12u
#define NP_EXEC_DATA 1u
#define NP_EXEC_RESIZE 2u
#define NP_EXEC_EXIT 3u
#define NP_EXEC_END_INPUT 4u
#define NP_EXEC_MAX_PAYLOAD (1u << 20)

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int exec_send_frame(int fd, uint8_t kind, const void *payload, uint32_t length) {
    uint8_t header[NP_EXEC_HEADER] = {'N', 'P', 'X', 'T', 1, kind, 0, 0};
    header[8] = (uint8_t)length;
    header[9] = (uint8_t)(length >> 8);
    header[10] = (uint8_t)(length >> 16);
    header[11] = (uint8_t)(length >> 24);
    if (np_write_full(fd, header, sizeof(header)) < 0) return -1;
    return length ? np_write_full(fd, payload, length) : 0;
}

static int exec_consume_input(struct exec_session *s, const uint8_t *bytes, size_t count) {
    if (count > sizeof(s->input) - s->input_len) return -1;
    memcpy(s->input + s->input_len, bytes, count);
    s->input_len += count;
    size_t off = 0;
    while (s->input_len - off >= NP_EXEC_HEADER) {
        const uint8_t *frame = s->input + off;
        if (memcmp(frame, "NPXT", 4) != 0 || frame[4] != 1) return -1;
        uint32_t length = read_le32(frame + 8);
        if (length > NP_EXEC_MAX_PAYLOAD) return -1;
        size_t total = NP_EXEC_HEADER + (size_t)length;
        if (s->input_len - off < total) break;
        const uint8_t *payload = frame + NP_EXEC_HEADER;
        if (frame[5] == NP_EXEC_DATA) {
            if (np_write_full(s->master_fd, payload, length) < 0) return -1;
        } else if (frame[5] == NP_EXEC_RESIZE && length == 8) {
            apply_pty_winsize(s->master_fd, read_le32(payload), read_le32(payload + 4));
        } else if (frame[5] == NP_EXEC_END_INPUT && length == 0) {
            struct termios attrs;
            unsigned char eof = 4;
            if (tcgetattr(s->master_fd, &attrs) == 0 &&
                attrs.c_cc[VEOF] != _POSIX_VDISABLE)
                eof = attrs.c_cc[VEOF];
            if (np_write_full(s->master_fd, &eof, 1) < 0) return -1;
        } else if (frame[5] != NP_EXEC_EXIT) {
            return -1;
        }
        off += total;
    }
    if (off) {
        memmove(s->input, s->input + off, s->input_len - off);
        s->input_len -= off;
    }
    return 0;
}

static void *exec_session_thread(void *arg) {
    struct exec_session *s = arg;
    if (!s)
        return NULL;
    int sock = -1;
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        sock = accept4(s->listen_fd, (struct sockaddr *)&ss, &sl, SOCK_CLOEXEC);
        if (sock < 0) {
            if (errno == EINTR)
                continue;
            logmsg("exec accept failed");
            break;
        }
        break;
    }
    close(s->listen_fd);
    s->listen_fd = -1;
    if (sock < 0) {
        close(s->master_fd);
        free(s);
        return NULL;
    }
    logmsg("exec vsock connected");

    uint8_t buf[8192];
    for (;;) {
        struct pollfd pfds[2];
        pfds[0].fd = sock;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = s->master_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;
            if (exec_consume_input(s, buf, (size_t)n) < 0)
                break;
        }
        if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* Drain any remaining PTY output, then stop. */
            ssize_t n = read(s->master_fd, buf, sizeof(buf));
            if (n > 0)
                exec_send_frame(sock, NP_EXEC_DATA, buf, (uint32_t)n);
            break;
        }
        if (pfds[1].revents & POLLIN) {
            ssize_t n = read(s->master_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;
            if (exec_send_frame(sock, NP_EXEC_DATA, buf, (uint32_t)n) < 0)
                break;
        }
    }
    int wait_status = 0;
    pid_t waited;
    do {
        waited = waitpid(s->pid, &wait_status, 0);
    } while (waited < 0 && errno == EINTR);
    int32_t exit_status = 1;
    if (waited >= 0 && WIFEXITED(wait_status))
        exit_status = WEXITSTATUS(wait_status);
    else if (waited >= 0 && WIFSIGNALED(wait_status))
        exit_status = 128 + WTERMSIG(wait_status);
    uint8_t exit_payload[4] = {
        (uint8_t)exit_status,
        (uint8_t)(exit_status >> 8),
        (uint8_t)(exit_status >> 16),
        (uint8_t)(exit_status >> 24),
    };
    exec_send_frame(sock, NP_EXEC_EXIT, exit_payload, sizeof(exit_payload));
    close(sock);
    close(s->master_fd);
    free(s);
    return NULL;
}

static pthread_mutex_t exec_port_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_exec_port = NP_PORT_SESSION_FIRST;

static int listen_exec_port(uint32_t *port_out) {
    pthread_mutex_lock(&exec_port_lock);
    for (uint32_t i = NP_PORT_SESSION_FIRST; i <= NP_PORT_SESSION_LAST; i++) {
        uint32_t port = next_exec_port++;
        if (next_exec_port > NP_PORT_SESSION_LAST)
            next_exec_port = NP_PORT_SESSION_FIRST;
        int fd = np_vsock_listen(port, 1);
        if (fd >= 0) {
            *port_out = port;
            pthread_mutex_unlock(&exec_port_lock);
            return fd;
        }
    }
    pthread_mutex_unlock(&exec_port_lock);
    return -1;
}

static int exec_spec(const struct launch_req *r, uint32_t cols, uint32_t rows,
                     int *pid_out, uint32_t *port_out) {
    if (!r || !r->exe || !r->exe[0] || !pid_out)
        return -1;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = cols ? (unsigned short)cols : 80;
    ws.ws_row = rows ? (unsigned short)rows : 24;

    int master = -1, slave = -1;
    if (openpty(&master, &slave, NULL, NULL, &ws) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        return -1;
    }
    if (pid == 0) {
        close(master);
        /* Equivalent to login_tty(slave): new session, controlling tty, stdio. */
        if (setsid() < 0)
            _exit(127);
        if (ioctl(slave, TIOCSCTTY, 0) < 0) {
            /* Non-fatal on some kernels if already controlling. */
        }
        if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
            _exit(127);
        if (slave > 2)
            close(slave);
        if (enter_launch_context(r) < 0)
            _exit(126);
        char *argv[NP_MAX_ARGS + 2];
        argv[0] = (char *)r->exe;
        for (int i = 0; i < r->nargs; i++)
            argv[i + 1] = (char *)r->args[i];
        argv[r->nargs + 1] = NULL;
        execvp(r->exe, argv);
        _exit(127);
    }
    close(slave);

    /* Listen before NPXS so the host can connect as soon as it gets the reply. */
    int listen_fd = listen_exec_port(port_out);
    if (listen_fd < 0) {
        kill(pid, SIGTERM);
        close(master);
        return -1;
    }

    struct exec_session *sess = malloc(sizeof(*sess));
    if (!sess) {
        kill(pid, SIGTERM);
        close(listen_fd);
        close(master);
        return -1;
    }
    sess->listen_fd = listen_fd;
    sess->master_fd = master;
    sess->pid = pid;
    sess->input_len = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, exec_session_thread, sess) != 0) {
        kill(pid, SIGTERM);
        close(listen_fd);
        close(master);
        free(sess);
        return -1;
    }
    pthread_detach(th);
    *pid_out = (int)pid;
    return 0;
}

static void do_shutdown(void) {
    char *cmds[][3] = {
        {"systemctl", "poweroff", NULL},
        {"poweroff", NULL, NULL},
        {"halt", "-p", NULL},
    };
    for (int i = 0; i < 3; i++) {
        if (np_run(cmds[i]) != 127)
            return;
    }
    sync();
}

static void *delayed_shutdown(void *arg) {
    (void)arg;
    usleep(250000);
    do_shutdown();
    return NULL;
}

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void wr_u64(uint8_t *p, uint64_t v) {
    wr_u32(p, (uint32_t)v);
    wr_u32(p + 4, (uint32_t)(v >> 32));
}

static void wr_i64(uint8_t *p, int64_t v) {
    wr_u64(p, (uint64_t)v);
}

static int rd_u16(const uint8_t *p, size_t n, size_t *off, uint16_t *out) {
    if (*off + 2 > n)
        return -1;
    *out = (uint16_t)p[*off] | ((uint16_t)p[*off + 1] << 8);
    *off += 2;
    return 0;
}

static int rd_u64(const uint8_t *p, size_t n, size_t *off, uint64_t *out) {
    if (*off + 8 > n)
        return -1;
    *out = (uint64_t)p[*off] | ((uint64_t)p[*off + 1] << 8) |
           ((uint64_t)p[*off + 2] << 16) | ((uint64_t)p[*off + 3] << 24) |
           ((uint64_t)p[*off + 4] << 32) | ((uint64_t)p[*off + 5] << 40) |
           ((uint64_t)p[*off + 6] << 48) | ((uint64_t)p[*off + 7] << 56);
    *off += 8;
    return 0;
}

static int send_bin_error(int fd, uint64_t id, uint32_t code, const char *msg) {
    size_t mlen = msg ? strlen(msg) : 0;
    if (mlen > 65535)
        mlen = 65535;
    size_t n = 4 + 8 + 4 + 2 + mlen;
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    memcpy(buf, "NPER", 4);
    wr_u64(buf + 4, id);
    wr_u32(buf + 12, code);
    wr_u16(buf + 16, (uint16_t)mlen);
    if (mlen)
        memcpy(buf + 18, msg, mlen);
    int rc = control_send(fd, buf, n);
    free(buf);
    return rc;
}

static int send_file_bytes(int fd, uint64_t id, const char *path, const uint8_t *data,
                           uint64_t size) {
    size_t plen = strlen(path);
    if (plen > 65535)
        plen = 65535;
    size_t n = 4 + 8 + 2 + plen + 8 + (size_t)size;
    if (n > NP_MAX_NPIP_PAYLOAD)
        return send_bin_error(fd, id, 27, "file too large");
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    uint8_t *p = buf;
    memcpy(p, "NPFL", 4);
    p += 4;
    wr_u64(p, id);
    p += 8;
    wr_u16(p, (uint16_t)plen);
    p += 2;
    memcpy(p, path, plen);
    p += plen;
    wr_u64(p, size);
    p += 8;
    if (size)
        memcpy(p, data, (size_t)size);
    int rc = control_send(fd, buf, n);
    free(buf);
    return rc;
}

static int send_dir_list(int fd, uint64_t id, const char *path, const uint8_t *entries,
                         size_t entries_len, uint32_t count, uint32_t flags) {
    size_t plen = strlen(path);
    if (plen > 65535)
        plen = 65535;
    size_t n = 4 + 8 + 2 + plen + 4 + 4 + entries_len;
    if (n > NP_MAX_NPIP_PAYLOAD)
        return send_bin_error(fd, id, 27, "listing too large");
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    uint8_t *p = buf;
    memcpy(p, "NPLS", 4);
    p += 4;
    wr_u64(p, id);
    p += 8;
    wr_u16(p, (uint16_t)plen);
    p += 2;
    memcpy(p, path, plen);
    p += plen;
    wr_u32(p, flags);
    p += 4;
    wr_u32(p, count);
    p += 4;
    if (entries_len)
        memcpy(p, entries, entries_len);
    int rc = control_send(fd, buf, n);
    free(buf);
    return rc;
}

struct list_hold {
    char *name;
    size_t len;
    size_t off;
    uint8_t dtype;
    int continues;
    int valid;
};

struct list_sess {
    int active;
    uint64_t id;
    DIR *dir;
    char path[4096];
    struct list_hold hold;
};

static void hold_clear(struct list_hold *h) {
    if (!h)
        return;
    free(h->name);
    memset(h, 0, sizeof(*h));
}

static int hold_set(struct list_hold *h, const char *name, size_t len, size_t off,
                    uint8_t dtype, int continues) {
    hold_clear(h);
    h->name = malloc(len + 1);
    if (!h->name)
        return -1;
    memcpy(h->name, name, len);
    h->name[len] = '\0';
    h->len = len;
    h->off = off;
    h->dtype = dtype;
    h->continues = continues;
    h->valid = 1;
    return 0;
}

static void list_sess_close(struct list_sess *s) {
    if (!s)
        return;
    if (s->dir)
        closedir(s->dir);
    hold_clear(&s->hold);
    s->dir = NULL;
    s->active = 0;
    s->id = 0;
}

static int list_append(uint8_t **buf, size_t *len, size_t *cap, uint32_t *count,
                       size_t *name_bytes, uint8_t dtype, uint8_t eflags, const char *name,
                       size_t nlen) {
    if (nlen == 0 || nlen > 65535)
        return 0;
    size_t need = 1 + 1 + 2 + nlen;
    if (*len + need > *cap) {
        size_t ncap = *cap ? *cap * 2 : 4096;
        while (ncap < *len + need)
            ncap *= 2;
        uint8_t *grown = realloc(*buf, ncap);
        if (!grown)
            return -1;
        *buf = grown;
        *cap = ncap;
    }
    uint8_t *p = *buf + *len;
    p[0] = dtype;
    p[1] = eflags;
    wr_u16(p + 2, (uint16_t)nlen);
    memcpy(p + 4, name, nlen);
    *len += need;
    *name_bytes += nlen;
    (*count)++;
    return 0;
}

/* Emit as much of s->hold as fits. Returns 1 if the chunk should stop.
 * Splits between names when possible; only cuts inside a name if that name
 * itself exceeds the remaining budget (or the u16 name_len limit). */
static int emit_hold(struct list_sess *s, uint8_t **buf, size_t *len, size_t *cap,
                     uint32_t *count, size_t *name_bytes, int *has_more,
                     int *split_in_name) {
    struct list_hold *h = &s->hold;
    if (!h->valid)
        return 0;
    size_t remain = h->len - h->off;
    if (remain == 0) {
        hold_clear(h);
        return 0;
    }
    size_t room = (*name_bytes < NP_LIST_NAME_BUDGET)
                      ? (NP_LIST_NAME_BUDGET - *name_bytes)
                      : 0;
    int mid_name = h->continues || h->off > 0;
    if (!mid_name && *count > 0 && remain > room) {
        *has_more = 1;
        *split_in_name = 0;
        return 1;
    }
    if (room == 0) {
        *has_more = 1;
        *split_in_name = mid_name;
        return 1;
    }
    size_t take = remain;
    if (take > room)
        take = room;
    if (take > 65535)
        take = 65535;
    int incomplete = take < remain;
    uint8_t eflags = 0;
    if (mid_name)
        eflags |= NP_ENTRY_CONT;
    if (incomplete)
        eflags |= NP_ENTRY_INCOMPLETE;
    if (list_append(buf, len, cap, count, name_bytes, h->dtype, eflags, h->name + h->off,
                    take) < 0)
        return -1;
    h->off += take;
    h->continues = 1;
    if (incomplete) {
        *has_more = 1;
        *split_in_name = 1;
        return 1;
    }
    hold_clear(h);
    return 0;
}

static int fill_list_chunk(struct list_sess *s, uint8_t **out, size_t *out_len,
                           uint32_t *count, int *has_more, int *split_in_name) {
    *has_more = 0;
    *split_in_name = 0;
    *count = 0;
    *out_len = 0;
    size_t cap = 4096, len = 0, name_bytes = 0;
    uint8_t *buf = malloc(cap);
    if (!buf)
        return -1;

    int stop = emit_hold(s, &buf, &len, &cap, count, &name_bytes, has_more, split_in_name);
    if (stop < 0) {
        free(buf);
        return -1;
    }
    if (stop) {
        *out = buf;
        *out_len = len;
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(s->dir))) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen == 0)
            continue;
        if (hold_set(&s->hold, ent->d_name, nlen, 0, (uint8_t)ent->d_type, 0) < 0) {
            free(buf);
            return -1;
        }
        stop = emit_hold(s, &buf, &len, &cap, count, &name_bytes, has_more, split_in_name);
        if (stop < 0) {
            free(buf);
            return -1;
        }
        if (stop) {
            *out = buf;
            *out_len = len;
            return 0;
        }
    }
    *out = buf;
    *out_len = len;
    return 0;
}

static int send_list_chunk(int fd, struct list_sess *s) {
    uint8_t *entries = NULL;
    size_t elen = 0;
    uint32_t count = 0;
    int has_more = 0, split_in_name = 0;
    if (fill_list_chunk(s, &entries, &elen, &count, &has_more, &split_in_name) < 0) {
        uint64_t id = s->id;
        int e = errno ? errno : 1;
        list_sess_close(s);
        return send_bin_error(fd, id, (uint32_t)e, strerror(e));
    }
    uint32_t flags = 0;
    if (has_more)
        flags |= NP_LIST_HAS_MORE;
    if (split_in_name)
        flags |= NP_LIST_NAME_TRUNC;
    int rc = send_dir_list(fd, s->id, s->path, entries, elen, count, flags);
    free(entries);
    if (!has_more)
        list_sess_close(s);
    return rc;
}

/* procfs and sysfs expose regular files whose stat size is zero even though a
 * read returns data.  Read every regular file to EOF under the protocol limit
 * instead of treating st_size as the payload length. */
static int read_regular_file(const char *path, size_t size_hint,
                             uint8_t **out, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    size_t capacity = size_hint ? size_hint : 4096;
    if (capacity > NP_MAX_READ)
        capacity = NP_MAX_READ;
    uint8_t *data = malloc(capacity ? capacity : 1);
    if (!data) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }

    size_t size = 0;
    for (;;) {
        if (size == capacity) {
            if (capacity == NP_MAX_READ) {
                uint8_t extra;
                ssize_t n;
                do {
                    n = read(fd, &extra, 1);
                } while (n < 0 && errno == EINTR);
                if (n > 0) errno = EFBIG;
                if (n != 0) goto fail;
                break;
            }
            size_t next = capacity > NP_MAX_READ / 2
                ? NP_MAX_READ : capacity * 2;
            uint8_t *grown = realloc(data, next);
            if (!grown) {
                errno = ENOMEM;
                goto fail;
            }
            data = grown;
            capacity = next;
        }

        ssize_t n = read(fd, data + size, capacity - size);
        if (n < 0) {
            if (errno == EINTR) continue;
            goto fail;
        }
        if (n == 0) break;
        size += (size_t)n;
    }

    close(fd);
    *out = data;
    *out_size = size;
    return 0;

fail: {
        int saved = errno ? errno : EIO;
        close(fd);
        free(data);
        errno = saved;
        return -1;
    }
}

static int handle_read_path(int fd, const uint8_t *payload, size_t n, struct list_sess *sess) {
    size_t off = 4;
    uint64_t id = 0;
    uint16_t plen = 0;
    if (rd_u64(payload, n, &off, &id) < 0 || rd_u16(payload, n, &off, &plen) < 0)
        return -1;
    if (off + plen > n || plen == 0 || plen >= 4096)
        return send_bin_error(fd, id, 22, "bad path");
    char path[4096];
    memcpy(path, payload + off, plen);
    path[plen] = '\0';
    if (path[0] != '/')
        return send_bin_error(fd, id, 22, "path must be absolute");

    list_sess_close(sess);

    struct stat st;
    if (stat(path, &st) < 0)
        return send_bin_error(fd, id, (uint32_t)errno, strerror(errno));

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir)
            return send_bin_error(fd, id, (uint32_t)errno, strerror(errno));
        sess->active = 1;
        sess->id = id;
        sess->dir = dir;
        snprintf(sess->path, sizeof(sess->path), "%s", path);
        return send_list_chunk(fd, sess);
    }
    if (!S_ISREG(st.st_mode))
        return send_bin_error(fd, id, 22, "not a file or directory");
    if ((uint64_t)st.st_size > NP_MAX_READ)
        return send_bin_error(fd, id, 27, "file too large");

    size_t size = 0;
    uint8_t *data = NULL;
    if (read_regular_file(path, (size_t)st.st_size, &data, &size) < 0) {
        int e = errno ? errno : EIO;
        return send_bin_error(fd, id, (uint32_t)e, strerror(e));
    }
    int rc = send_file_bytes(fd, id, path, data, size);
    free(data);
    return rc;
}

static int handle_list_continue(int fd, const uint8_t *payload, size_t n,
                                struct list_sess *sess) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    if (!sess->active || sess->id != id || !sess->dir)
        return send_bin_error(fd, id, 2, "no listing in progress");
    return send_list_chunk(fd, sess);
}

static int handle_list_cancel(const uint8_t *payload, size_t n, struct list_sess *sess) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    if (sess->active && sess->id == id)
        list_sess_close(sess);
    return 0;
}

static int rd_u32(const uint8_t *p, size_t n, size_t *off, uint32_t *out) {
    if (*off + 4 > n)
        return -1;
    *out = (uint32_t)p[*off] | ((uint32_t)p[*off + 1] << 8) |
           ((uint32_t)p[*off + 2] << 16) | ((uint32_t)p[*off + 3] << 24);
    *off += 4;
    return 0;
}

static int rd_bytes(const uint8_t *p, size_t n, size_t *off, size_t len, const uint8_t **out) {
    if (*off + len > n)
        return -1;
    *out = p + *off;
    *off += len;
    return 0;
}

static int rd_str(const uint8_t *p, size_t n, size_t *off, char *out, size_t cap) {
    uint16_t len = 0;
    if (rd_u16(p, n, off, &len) < 0)
        return -1;
    if (len >= cap || *off + len > n)
        return -1;
    memcpy(out, p + *off, len);
    out[len] = '\0';
    *off += len;
    return 0;
}

static int rd_str_alloc(const uint8_t *p, size_t n, size_t *off, char **out) {
    uint16_t len = 0;
    if (!out || rd_u16(p, n, off, &len) < 0 || *off + len > n)
        return -1;
    // exec/setenv consume C strings; accepting an embedded NUL would make the
    // validated wire value differ from the value actually used by the child.
    if (memchr(p + *off, '\0', len))
        return -1;
    char *value = malloc((size_t)len + 1);
    if (!value)
        return -1;
    memcpy(value, p + *off, len);
    value[len] = '\0';
    *off += len;
    *out = value;
    return 0;
}

static int parse_launch_req(const uint8_t *p, size_t n, size_t *off, struct launch_req *r) {
    launch_req_clear(r);
    if (rd_str_alloc(p, n, off, &r->exe) < 0)
        return -1;
    if (rd_str_alloc(p, n, off, &r->cwd) < 0)
        return -1;
    if (r->cwd[0])
        r->have_cwd = 1;
    uint16_t nargs = 0;
    if (rd_u16(p, n, off, &nargs) < 0 || nargs > NP_MAX_ARGS)
        return -1;
    for (uint16_t i = 0; i < nargs; i++) {
        if (rd_str_alloc(p, n, off, &r->args[i]) < 0)
            return -1;
    }
    r->nargs = (int)nargs;
    uint16_t nenv = 0;
    if (rd_u16(p, n, off, &nenv) < 0 || nenv > NP_MAX_ENV)
        return -1;
    for (uint16_t i = 0; i < nenv; i++) {
        if (rd_str_alloc(p, n, off, &r->env_keys[i]) < 0 ||
            rd_str_alloc(p, n, off, &r->env_vals[i]) < 0)
            return -1;
    }
    r->nenv = (int)nenv;
    // Compatibility with guestd 0.2.3 clients, whose launch payload ended at
    // the environment list.  Current clients append a u32 stdin length.
    if (*off == n)
        return 0;
    uint32_t stdin_len = 0;
    if (rd_u32(p, n, off, &stdin_len) < 0 || stdin_len > NP_MAX_STDIN)
        return -1;
    if (stdin_len) {
        const uint8_t *bytes = NULL;
        if (rd_bytes(p, n, off, stdin_len, &bytes) < 0)
            return -1;
        r->stdin_data = malloc(stdin_len);
        if (!r->stdin_data)
            return -1;
        memcpy(r->stdin_data, bytes, stdin_len);
        r->stdin_len = stdin_len;
    }
    // 0.2.6 extension. Older hosts end immediately after stdin.
    if (*off == n)
        return 0;
    if (rd_str_alloc(p, n, off, &r->username) < 0)
        return -1;
    return *off == n ? 0 : -1;
}

static size_t guest_info_encoded_size(const struct guest_info *gi) {
    const char *fields[] = {gi->agent_version, gi->kernel_release, gi->distro_name,
                            gi->distro_version, gi->init_system};
    const char *caps[] = {
        "console.resize", "process.launch", "process.run", "process.exec", "fs.read", "fs.stat", "fs.write",
        "account.credentials", "agent.version", "environment.catalog", "resource.sync.v1",
        "integration.desktop-preferences",
        "display.wayland",
    };
    int ncap = gi->have_wayland ? 13 : 12;
    size_t n = 2;
    for (int i = 0; i < 5; i++)
        n += 2 + strlen(fields[i]);
    for (int i = 0; i < ncap; i++)
        n += 2 + strlen(caps[i]);
    char revision[32];
    snprintf(revision, sizeof(revision), "%llu",
             (unsigned long long)gi->environment_revision);
    n += 2 + strlen(gi->environment_profile) + 2 + strlen(revision) +
         2 + strlen(gi->os_id) + 2 + strlen(gi->os_id_like) +
         2 + strlen(gi->architecture);
    return n;
}

static size_t encode_guest_info(uint8_t *p, const struct guest_info *gi) {
    const char *fields[] = {gi->agent_version, gi->kernel_release, gi->distro_name,
                            gi->distro_version, gi->init_system};
    const char *caps[] = {
        "console.resize", "process.launch", "process.run", "process.exec", "fs.read", "fs.stat", "fs.write",
        "account.credentials", "agent.version", "environment.catalog", "resource.sync.v1",
        "integration.desktop-preferences",
        "display.wayland",
    };
    int ncap = gi->have_wayland ? 13 : 12;
    uint8_t *start = p;
    for (int i = 0; i < 5; i++) {
        size_t len = strlen(fields[i]);
        if (len > 65535)
            len = 65535;
        wr_u16(p, (uint16_t)len);
        p += 2;
        memcpy(p, fields[i], len);
        p += len;
    }
    wr_u16(p, (uint16_t)ncap);
    p += 2;
    for (int i = 0; i < ncap; i++) {
        size_t len = strlen(caps[i]);
        wr_u16(p, (uint16_t)len);
        p += 2;
        memcpy(p, caps[i], len);
        p += len;
    }
    const char *environment_fields[] = {
        gi->environment_profile, NULL, gi->os_id, gi->os_id_like, gi->architecture,
    };
    char revision[32];
    snprintf(revision, sizeof(revision), "%llu",
             (unsigned long long)gi->environment_revision);
    environment_fields[1] = revision;
    for (int i = 0; i < 5; i++) {
        size_t len = strlen(environment_fields[i]);
        wr_u16(p, (uint16_t)len);
        p += 2;
        memcpy(p, environment_fields[i], len);
        p += len;
    }
    return (size_t)(p - start);
}

static int send_ok(int fd, uint64_t id) {
    uint8_t buf[12];
    memcpy(buf, "NPOK", 4);
    wr_u64(buf + 4, id);
    return control_send(fd, buf, sizeof(buf));
}

static int handle_desktop_preferences(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0 || off + 1 != n)
        return send_bin_error(fd, id, 22, "bad desktop preferences");
    uint8_t color_scheme = payload[off];
    if (color_scheme != 1 && color_scheme != 2)
        return send_bin_error(fd, id, 22, "bad color scheme");
    const char *text = color_scheme == 2
        ? "color-scheme=dark\n" : "color-scheme=light\n";
    if (np_write_file(NP_DESKTOP_PREFERENCES_FILE, text, strlen(text), 0644) < 0)
        return send_bin_error(fd, id, (uint32_t)(errno ? errno : EIO),
                              "cannot persist desktop preferences");
    apply_desktop_preferences_to_session(color_scheme);
    return send_ok(fd, id);
}

static int send_info(int fd, uint64_t id, const struct guest_info *gi) {
    size_t info_n = guest_info_encoded_size(gi);
    size_t n = 4 + 8 + info_n;
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    memcpy(buf, "NPIF", 4);
    wr_u64(buf + 4, id);
    encode_guest_info(buf + 12, gi);
    int rc = control_send(fd, buf, n);
    free(buf);
    return rc;
}

static int send_runtime_ready(int fd, const struct guest_info *gi) {
    size_t info_n = guest_info_encoded_size(gi);
    size_t n = 4 + info_n;
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    memcpy(buf, "NPRT", 4);
    encode_guest_info(buf + 4, gi);
    int rc = control_send(fd, buf, n);
    free(buf);
    return rc;
}

static int send_launched(int fd, uint64_t id, int32_t pid) {
    uint8_t buf[16];
    memcpy(buf, "NPLP", 4);
    wr_u64(buf + 4, id);
    wr_u32(buf + 12, (uint32_t)pid);
    return control_send(fd, buf, sizeof(buf));
}

static int send_process_exited(int fd, int32_t pid, int32_t status) {
    uint8_t buf[12];
    memcpy(buf, "NPEX", 4);
    wr_u32(buf + 4, (uint32_t)pid);
    wr_u32(buf + 8, (uint32_t)status);
    return control_send(fd, buf, sizeof(buf));
}

static int send_exec_session(int fd, uint64_t id, int32_t pid, uint32_t port) {
    uint8_t buf[20];
    memcpy(buf, "NPXS", 4);
    wr_u64(buf + 4, id);
    wr_u32(buf + 12, (uint32_t)pid);
    wr_u32(buf + 16, port);
    return control_send(fd, buf, sizeof(buf));
}

static int send_ran(int fd, uint64_t id, int32_t status, const uint8_t *out, size_t out_n,
                    const uint8_t *err, size_t err_n) {
    if (out_n > 0xffffffffu || err_n > 0xffffffffu)
        return send_bin_error(fd, id, 27, "output too large");
    size_t n = 4 + 8 + 4 + 4 + out_n + 4 + err_n;
    if (n > NP_MAX_NPIP_PAYLOAD)
        return send_bin_error(fd, id, 27, "output too large");
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    uint8_t *p = buf;
    memcpy(p, "NPRX", 4);
    p += 4;
    wr_u64(p, id);
    p += 8;
    wr_u32(p, (uint32_t)status);
    p += 4;
    wr_u32(p, (uint32_t)out_n);
    p += 4;
    if (out_n)
        memcpy(p, out, out_n);
    p += out_n;
    wr_u32(p, (uint32_t)err_n);
    p += 4;
    if (err_n)
        memcpy(p, err, err_n);
    int rc = control_send(fd, buf, n);
    free(buf);
    return rc;
}

static int send_path_stat(int fd, uint64_t id, const char *path, const struct stat *st) {
    size_t plen = strlen(path);
    if (plen > 65535)
        plen = 65535;
    size_t n = 4 + 8 + 2 + plen + 4 + 4 + 4 + 8 + 8;
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    uint8_t *p = buf;
    memcpy(p, "NPFS", 4);
    p += 4;
    wr_u64(p, id);
    p += 8;
    wr_u16(p, (uint16_t)plen);
    p += 2;
    memcpy(p, path, plen);
    p += plen;
    wr_u32(p, (uint32_t)st->st_mode);
    p += 4;
    wr_u32(p, (uint32_t)st->st_uid);
    p += 4;
    wr_u32(p, (uint32_t)st->st_gid);
    p += 4;
    wr_u64(p, (uint64_t)st->st_size);
    p += 8;
    wr_i64(p, (int64_t)st->st_mtime);
    int rc = control_send(fd, buf, n);
    free(buf);
    return rc;
}

static int handle_hello(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    char ver[256];
    if (rd_str(payload, n, &off, ver, sizeof(ver)) < 0)
        return send_bin_error(fd, id, 22, "bad hello");
    (void)ver;
    struct guest_info gi;
    fill_guest_info(&gi);
    return send_info(fd, id, &gi);
}

static int handle_ping(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    return send_ok(fd, id);
}

static int send_version(int fd, uint64_t id) {
    char ver[NP_MAX_VERSION];
    guest_version(ver, sizeof(ver));
    size_t len = strlen(ver);
    if (len > 65535)
        len = 65535;
    size_t n = 4 + 8 + 2 + len;
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    memcpy(buf, "NPVV", 4);
    wr_u64(buf + 4, id);
    wr_u16(buf + 12, (uint16_t)len);
    memcpy(buf + 14, ver, len);
    int rc = control_send(fd, buf, n);
    free(buf);
    return rc;
}

static int handle_get_version(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    return send_version(fd, id);
}

static int handle_environment_refresh(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0 || off != n)
        return -1;
    if (refresh_current_environment_profile(1) < 0)
        return send_bin_error(fd, id, (uint32_t)(errno ? errno : 1),
                              "environment refresh failed");
    struct guest_info gi;
    fill_guest_info(&gi);
    return send_info(fd, id, &gi);
}

static int reconcile_guestd_binary(const char *desired_version) {
    char current[NP_MAX_VERSION];
    guest_version(current, sizeof(current));
    if (!desired_version[0] || strcmp(current, desired_version) == 0)
        return 0;
    int agent = np_vsock_connect_host(NP_PORT_AGENT, 3);
    if (agent < 0)
        return -1;
    if (np_agent_send_request(agent, NP_GUESTD_NAME, current) < 0) {
        close(agent);
        return -1;
    }
    np_agent_hdr hdr;
    if (np_agent_recv_hdr(agent, &hdr) < 0) {
        close(agent);
        return -1;
    }
    if ((hdr.status != NP_STATUS_FILE && hdr.status != NP_STATUS_FORCE) ||
        strcmp(hdr.version, desired_version) != 0) {
        if (hdr.payload_len)
            np_agent_discard_payload(agent, hdr.payload_len);
        close(agent);
        errno = ESTALE;
        return -1;
    }
    int rc = apply_guestd_from_fd(agent, &hdr);
    close(agent);
    return rc < 0 ? -1 : 1;
}

static void *delayed_reexec(void *unused) {
    (void)unused;
    sleep(1);
    reexec();
    return NULL;
}

/* NPSY: one host-selected desired state for guestd + environment resources.
 * The heavy pull/apply work runs on a worker so the control reader remains
 * responsive to ping, resize and cancellation frames. */
static int handle_resource_sync(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0, revision = 0;
    char guestd_version[NP_MAX_VERSION];
    char profile_id[NP_ENV_MAX_PROFILE_ID];
    if (rd_u64(payload, n, &off, &id) < 0 ||
        rd_str(payload, n, &off, guestd_version, sizeof(guestd_version)) < 0 ||
        rd_u64(payload, n, &off, &revision) < 0 ||
        rd_str(payload, n, &off, profile_id, sizeof(profile_id)) < 0 || off != n)
        return send_bin_error(fd, id, 22, "bad resource desired state");

    pthread_mutex_lock(&resource_sync_lock);
    int updated = reconcile_guestd_binary(guestd_version);
    if (updated < 0) {
        int saved = errno;
        pthread_mutex_unlock(&resource_sync_lock);
        return send_bin_error(fd, id, (uint32_t)(saved ? saved : 1),
                              "guestd resource sync failed");
    }
    if (updated > 0) {
        int rc = send_ok(fd, id);
        pthread_mutex_unlock(&resource_sync_lock);
        pthread_t th;
        if (pthread_create(&th, NULL, delayed_reexec, NULL) == 0)
            pthread_detach(th);
        else
            reexec();
        return rc;
    }
    if (reconcile_environment_profile(profile_id, revision, 1) < 0) {
        int saved = errno;
        pthread_mutex_unlock(&resource_sync_lock);
        return send_bin_error(fd, id, (uint32_t)(saved ? saved : 1),
                              "environment desired state rejected");
    }
    struct guest_info gi;
    fill_guest_info(&gi);
    int rc = send_info(fd, id, &gi);
    pthread_mutex_unlock(&resource_sync_lock);
    return rc;
}

static int handle_resize(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    uint32_t cols = 0, rows = 0;
    if (rd_u64(payload, n, &off, &id) < 0 || rd_u32(payload, n, &off, &cols) < 0 ||
        rd_u32(payload, n, &off, &rows) < 0)
        return -1;
    if (resize_console((int)cols, (int)rows) < 0)
        return send_bin_error(fd, id, (uint32_t)errno, "resize failed");
    return send_ok(fd, id);
}

static int handle_launch(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    struct launch_req req;
    memset(&req, 0, sizeof(req));
    if (parse_launch_req(payload, n, &off, &req) < 0) {
        launch_req_clear(&req);
        return send_bin_error(fd, id, 22, "bad launch");
    }
    int pid = 0;
    int rc = launch_spec(&req, &pid);
    launch_req_clear(&req);
    if (rc < 0) {
        int failure = errno ? errno : EINVAL;
        return send_bin_error(fd, id, (uint32_t)failure, strerror(failure));
    }
    rc = send_launched(fd, id, pid);
    reap_child_async(pid, fd);
    return rc;
}

static int handle_run(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    struct launch_req req;
    memset(&req, 0, sizeof(req));
    if (parse_launch_req(payload, n, &off, &req) < 0) {
        launch_req_clear(&req);
        return send_bin_error(fd, id, 22, "bad run");
    }
    int status = 0;
    uint8_t *outb = NULL, *errb = NULL;
    size_t outn = 0, errn = 0;
    int rc = run_spec(&req, &status, &outb, &outn, &errb, &errn);
    launch_req_clear(&req);
    if (rc < 0) {
        free(outb);
        free(errb);
        return send_bin_error(fd, id, (uint32_t)(errno ? errno : 1), "run failed");
    }
    rc = send_ran(fd, id, status, outb, outn, errb, errn);
    free(outb);
    free(errb);
    return rc;
}

static int handle_exec(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    uint32_t cols = 0, rows = 0;
    if (rd_u64(payload, n, &off, &id) < 0 || rd_u32(payload, n, &off, &cols) < 0 ||
        rd_u32(payload, n, &off, &rows) < 0)
        return -1;
    struct launch_req req;
    memset(&req, 0, sizeof(req));
    if (parse_launch_req(payload, n, &off, &req) < 0) {
        launch_req_clear(&req);
        return send_bin_error(fd, id, 22, "bad exec");
    }
    int pid = 0;
    uint32_t port = 0;
    int rc = exec_spec(&req, cols, rows, &pid, &port);
    launch_req_clear(&req);
    if (rc < 0)
        return send_bin_error(fd, id, (uint32_t)(errno ? errno : 22), "exec failed");
    return send_exec_session(fd, id, pid, port);
}

static int handle_shutdown(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    send_ok(fd, id);
    pthread_t th;
    pthread_create(&th, NULL, delayed_shutdown, NULL);
    pthread_detach(th);
    return 0;
}

static int handle_set_user(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    char user[64], old_user[64];
    if (rd_str(payload, n, &off, user, sizeof(user)) < 0)
        return send_bin_error(fd, id, 22, "missing username");
    if (off >= n)
        return send_bin_error(fd, id, 22, "bad setUser");
    uint8_t flags = payload[off++];
    int have_old = 0;
    if (flags & NP_SETUSER_HAS_OLD) {
        if (rd_str(payload, n, &off, old_user, sizeof(old_user)) < 0)
            return send_bin_error(fd, id, 22, "bad oldUsername");
        have_old = 1;
    }
    if (set_user(user, have_old ? old_user : NULL) < 0)
        return send_bin_error(fd, id, (uint32_t)(errno ? errno : 1), "setUser failed");
    prepare_graphical_session(user);
    return send_ok(fd, id);
}

static int handle_set_password(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    char user[64], pass[256];
    if (rd_str(payload, n, &off, user, sizeof(user)) < 0 ||
        rd_str(payload, n, &off, pass, sizeof(pass)) < 0)
        return send_bin_error(fd, id, 22, "missing username/password");
    if (set_password(user, pass) < 0)
        return send_bin_error(fd, id, (uint32_t)(errno ? errno : 1), "setPassword failed");
    return send_ok(fd, id);
}

static int handle_stat_path(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0;
    if (rd_u64(payload, n, &off, &id) < 0)
        return -1;
    char path[4096];
    if (rd_str(payload, n, &off, path, sizeof(path)) < 0 || path[0] != '/')
        return send_bin_error(fd, id, 22, "path must be absolute");
    struct stat st;
    if (lstat(path, &st) < 0)
        return send_bin_error(fd, id, (uint32_t)errno, "stat failed");
    return send_path_stat(fd, id, path, &st);
}

static int handle_write_path(int fd, const uint8_t *payload, size_t n) {
    size_t off = 4;
    uint64_t id = 0, size = 0;
    uint32_t mode = 0;
    char path[4096];
    const uint8_t *data = NULL;
    if (rd_u64(payload, n, &off, &id) < 0 ||
        rd_str(payload, n, &off, path, sizeof(path)) < 0 || path[0] != '/' ||
        rd_u32(payload, n, &off, &mode) < 0 || rd_u64(payload, n, &off, &size) < 0 ||
        size > SIZE_MAX || rd_bytes(payload, n, &off, (size_t)size, &data) < 0 || off != n)
        return send_bin_error(fd, id, 22, "bad writePath");
    if (np_write_file(path, data, (size_t)size, (int)(mode & 07777u)) < 0)
        return send_bin_error(fd, id, (uint32_t)(errno ? errno : 5), "write failed");
    return send_ok(fd, id);
}

struct control_job {
    int fd;
    uint8_t *payload;
    size_t len;
};

static int is_async_control_request(const uint8_t *payload) {
    return memcmp(payload, "NPSY", 4) == 0 ||
           memcmp(payload, "NPDP", 4) == 0 ||
           memcmp(payload, "NPEU", 4) == 0 ||
           memcmp(payload, "NPRU", 4) == 0 ||
           memcmp(payload, "NPXC", 4) == 0 ||
           memcmp(payload, "NPWR", 4) == 0 ||
           memcmp(payload, "NPUS", 4) == 0 ||
           memcmp(payload, "NPWP", 4) == 0;
}

static void *serve_control_job(void *arg) {
    struct control_job *job = arg;
    const uint8_t *payload = job->payload;
    size_t n = job->len;
    if (memcmp(payload, "NPSY", 4) == 0)
        handle_resource_sync(job->fd, payload, n);
    else if (memcmp(payload, "NPDP", 4) == 0)
        handle_desktop_preferences(job->fd, payload, n);
    else if (memcmp(payload, "NPEU", 4) == 0)
        handle_environment_refresh(job->fd, payload, n);
    else if (memcmp(payload, "NPRU", 4) == 0)
        handle_run(job->fd, payload, n);
    else if (memcmp(payload, "NPXC", 4) == 0)
        handle_exec(job->fd, payload, n);
    else if (memcmp(payload, "NPWR", 4) == 0)
        handle_write_path(job->fd, payload, n);
    else if (memcmp(payload, "NPUS", 4) == 0)
        handle_set_user(job->fd, payload, n);
    else if (memcmp(payload, "NPWP", 4) == 0)
        handle_set_password(job->fd, payload, n);
    free(job->payload);
    close(job->fd);
    free(job);
    return NULL;
}

/* The reader owns framing and never waits for a long operation. Request IDs
 * allow replies to complete out of order; control_send() preserves frame
 * atomicity across workers. */
static int dispatch_control_job(int fd, uint8_t *payload, size_t len) {
    struct control_job *job = calloc(1, sizeof(*job));
    if (!job)
        return -1;
    job->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (job->fd < 0) {
        free(job);
        return -1;
    }
    job->payload = payload;
    job->len = len;
    pthread_t th;
    if (pthread_create(&th, NULL, serve_control_job, job) != 0) {
        close(job->fd);
        free(job);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

static void *serve_session(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct list_sess listing;
    memset(&listing, 0, sizeof(listing));
    struct guest_info gi;
    fill_guest_info(&gi);
    send_runtime_ready(fd, &gi);

    for (;;) {
        uint8_t *payload = NULL;
        ssize_t n = np_npip_recv(fd, &payload);
        if (n < 0) {
            free(payload);
            break;
        }
        if (n < 4) {
            free(payload);
            continue;
        }
        if (is_async_control_request(payload) &&
            dispatch_control_job(fd, payload, (size_t)n) == 0)
            continue;
        if (memcmp(payload, "NPHI", 4) == 0)
            handle_hello(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPPG", 4) == 0)
            handle_ping(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPVQ", 4) == 0)
            handle_get_version(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPEU", 4) == 0)
            handle_environment_refresh(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPSY", 4) == 0)
            handle_resource_sync(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPDP", 4) == 0)
            handle_desktop_preferences(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPRZ", 4) == 0)
            handle_resize(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPLN", 4) == 0)
            handle_launch(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPRU", 4) == 0)
            handle_run(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPXC", 4) == 0)
            handle_exec(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPRE", 4) == 0)
            handle_read_path(fd, payload, (size_t)n, &listing);
        else if (memcmp(payload, "NPWR", 4) == 0)
            handle_write_path(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPCT", 4) == 0)
            handle_list_continue(fd, payload, (size_t)n, &listing);
        else if (memcmp(payload, "NPCL", 4) == 0)
            handle_list_cancel(payload, (size_t)n, &listing);
        else if (memcmp(payload, "NPMS", 4) == 0)
            handle_stat_path(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPSH", 4) == 0)
            handle_shutdown(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPUS", 4) == 0)
            handle_set_user(fd, payload, (size_t)n);
        else if (memcmp(payload, "NPWP", 4) == 0)
            handle_set_password(fd, payload, (size_t)n);
        else
            logmsg("unknown control magic");
        free(payload);
    }
    list_sess_close(&listing);
    close(fd);
    return NULL;
}

static int listen_control(void) {
    int fd = np_vsock_listen(NP_PORT_CONTROL, 4);
    if (fd < 0) {
        logmsg("listen control failed");
        return 1;
    }
    logmsg("listening on vsock port 1024");
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        int cfd = accept4(fd, (struct sockaddr *)&ss, &sl, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        logmsg("control connection");
        pthread_t th;
        pthread_create(&th, NULL, serve_session, (void *)(intptr_t)cfd);
        pthread_detach(th);
    }
    close(fd);
    return 1;
}

int main(int argc, char **argv) {
    /* A disconnected host is an ordinary transport error, not a reason to
     * terminate the entire guest agent while another session is active. */
    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("%s\n", NP_GUESTD_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--provision") == 0)
            return provision();
    }
    load_cached_environment_catalog();
    /* Also run once per guestd/catalog combination after an in-place upgrade.
     * The stamp prevents the service started by --provision from regenerating
     * GRUB a second time while still allowing a catalog revision to reapply. */
    if (geteuid() == 0 && has_host_environment_policy()) {
        struct np_environment_policy policy;
        current_environment_policy(&policy);
        if (policy.flags & NP_ENV_CLOUD_INIT_RECOVERY)
            ensure_cloud_init_recovery_policy();
        if (policy.flags & NP_ENV_REPAIR_CLOUD_INIT_NETWORK)
            repair_cloud_init_cache();
        if (!console_integration_is_current()) {
            char init[64];
            detect_init(init, sizeof(init));
            ensure_console_integration(init, 1);
        }
    }
    boot_self_update();
    {
        pthread_t th;
        if (pthread_create(&th, NULL, mount_host_shares, NULL) == 0)
            pthread_detach(th);
        else
            logmsg("cannot start host shared-folder mount worker");
    }
    {
        pthread_t th;
        pthread_create(&th, NULL, session_stack_thread, NULL);
        pthread_detach(th);
    }
    return listen_control();
}
