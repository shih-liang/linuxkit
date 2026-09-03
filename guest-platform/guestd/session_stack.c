#include "session_stack.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void clear_file_state(struct np_session_stack_file_state *state) {
    state->backup_path[0] = '\0';
    state->had_original = 0;
    state->committed = 0;
}

int np_session_stack_commit_file(
    const struct np_session_stack_artifact *artifact,
    const char *staging_path,
    const char *backup_path,
    struct np_session_stack_file_state *state) {
    if (!artifact || !artifact->destination || !staging_path || !staging_path[0] ||
        !backup_path || !backup_path[0] || !state || state->committed ||
        state->backup_path[0]) {
        errno = EINVAL;
        return -1;
    }
    int written = snprintf(state->backup_path, sizeof(state->backup_path),
                           "%s", backup_path);
    if (written < 0 || written >= (int)sizeof(state->backup_path)) {
        clear_file_state(state);
        errno = ENAMETOOLONG;
        return -1;
    }
    struct stat existing;
    if (lstat(state->backup_path, &existing) == 0) {
        clear_file_state(state);
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        int saved = errno;
        clear_file_state(state);
        errno = saved;
        return -1;
    }
    if (rename(artifact->destination, state->backup_path) == 0) {
        state->had_original = 1;
    } else if (errno != ENOENT) {
        int saved = errno;
        clear_file_state(state);
        errno = saved;
        return -1;
    }
    if (rename(staging_path, artifact->destination) < 0) {
        int saved = errno;
        if (state->had_original &&
            rename(state->backup_path, artifact->destination) < 0) {
            state->committed = 1;
            return -1;
        }
        clear_file_state(state);
        errno = saved;
        return -1;
    }
    state->committed = 1;
    return 0;
}

int np_session_stack_rollback_file(
    const struct np_session_stack_artifact *artifact,
    struct np_session_stack_file_state *state) {
    if (!artifact || !artifact->destination || !state) {
        errno = EINVAL;
        return -1;
    }
    if (!state->committed)
        return 0;
    int rc;
    if (state->had_original)
        rc = rename(state->backup_path, artifact->destination);
    else
        rc = unlink(artifact->destination) == 0 || errno == ENOENT ? 0 : -1;
    if (rc == 0)
        clear_file_state(state);
    return rc;
}

int np_session_stack_finish_file(struct np_session_stack_file_state *state) {
    if (!state) {
        errno = EINVAL;
        return -1;
    }
    if (!state->committed)
        return 0;
    if (state->had_original && unlink(state->backup_path) < 0 && errno != ENOENT)
        return -1;
    clear_file_state(state);
    return 0;
}

static int run_systemd(const struct np_session_stack_operations *operations) {
    const char *reload[] = {"systemctl", "daemon-reload", NULL};
    const char *enable[] = {
        "systemctl", "enable", "nativepipe-session.service", NULL,
    };
    const char *restart[] = {
        "systemctl", "restart", "nativepipe-session.service", NULL,
    };
    if (operations->run(operations->context, reload) != 0)
        return -1;
    if (operations->run(operations->context, enable) != 0)
        return -1;
    return operations->run(operations->context, restart) == 0 ? 0 : -1;
}

static int run_openrc(const struct np_session_stack_operations *operations) {
    const char *enable[] = {
        "rc-update", "add", "nativepipe-session", "default", NULL,
    };
    const char *restart[] = {
        "rc-service", "nativepipe-session", "restart", NULL,
    };
    if (operations->run(operations->context, enable) != 0)
        return -1;
    return operations->run(operations->context, restart) == 0 ? 0 : -1;
}

static int rollback_artifacts(
    const struct np_session_stack_artifact *artifacts,
    size_t count,
    const struct np_session_stack_operations *operations) {
    int failed = 0;
    while (count > 0) {
        count--;
        if (operations->rollback(
                operations->context, &artifacts[count], count) != 0)
            failed = 1;
    }
    return failed ? -1 : 0;
}

static int restore_systemd(
    const struct np_session_stack_operations *operations) {
    const char *reload[] = {"systemctl", "daemon-reload", NULL};
    const char *restart[] = {
        "systemctl", "restart", "nativepipe-session.service", NULL,
    };
    int failed = 0;
    if (operations->run(operations->context, reload) != 0)
        failed = 1;
    if (operations->run(operations->context, restart) != 0)
        failed = 1;
    return failed ? -1 : 0;
}

static int restore_openrc(
    const struct np_session_stack_operations *operations) {
    const char *restart[] = {
        "rc-service", "nativepipe-session", "restart", NULL,
    };
    return operations->run(operations->context, restart) == 0 ? 0 : -1;
}

int np_session_stack_apply(
    const struct np_session_stack_artifact *artifacts,
    size_t artifact_count,
    enum np_session_stack_service service,
    const struct np_session_stack_operations *operations) {
    if (!artifacts || artifact_count == 0 ||
        (service != NP_SESSION_STACK_SYSTEMD &&
         service != NP_SESSION_STACK_OPENRC) || !operations ||
        !operations->stage || !operations->commit || !operations->rollback ||
        !operations->finish || !operations->discard || !operations->run) {
        errno = EINVAL;
        return -1;
    }
    size_t staged = 0;
    for (; staged < artifact_count; staged++) {
        if (operations->stage(
                operations->context, &artifacts[staged], staged) != 0) {
            while (staged > 0) {
                staged--;
                operations->discard(
                    operations->context, &artifacts[staged], staged);
            }
            return -1;
        }
    }
    for (size_t index = 0; index < artifact_count; index++) {
        if (operations->commit(
                operations->context, &artifacts[index], index) != 0) {
            int commit_errno = errno ? errno : EIO;
            for (size_t pending = index; pending < artifact_count; pending++)
                operations->discard(
                    operations->context, &artifacts[pending], pending);
            int rollback_failed = rollback_artifacts(
                artifacts, index + 1, operations) != 0;
            errno = rollback_failed ? EIO : commit_errno;
            return -1;
        }
    }
    int service_result = service == NP_SESSION_STACK_SYSTEMD
        ? run_systemd(operations) : run_openrc(operations);
    if (service_result != 0) {
        int service_errno = errno ? errno : EIO;
        int rollback_failed = rollback_artifacts(
            artifacts, artifact_count, operations) != 0;
        int restore_failed = service == NP_SESSION_STACK_SYSTEMD
            ? restore_systemd(operations) != 0
            : restore_openrc(operations) != 0;
        errno = rollback_failed || restore_failed ? EIO : service_errno;
        return -1;
    }
    for (size_t index = 0; index < artifact_count; index++)
        operations->finish(operations->context, &artifacts[index], index);
    return 0;
}
