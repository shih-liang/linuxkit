#ifndef NATIVEPIPE_SESSION_STACK_H
#define NATIVEPIPE_SESSION_STACK_H

#include <stddef.h>

enum np_session_stack_service {
    NP_SESSION_STACK_SYSTEMD = 1,
    NP_SESSION_STACK_OPENRC = 2,
};

struct np_session_stack_artifact {
    const char *name;
    const char *destination;
    int mode;
};

#define NP_SESSION_STACK_PATH_CAP 1024

struct np_session_stack_file_state {
    char backup_path[NP_SESSION_STACK_PATH_CAP];
    unsigned char had_original;
    unsigned char committed;
};

int np_session_stack_commit_file(
    const struct np_session_stack_artifact *artifact,
    const char *staging_path,
    const char *backup_path,
    struct np_session_stack_file_state *state);
int np_session_stack_rollback_file(
    const struct np_session_stack_artifact *artifact,
    struct np_session_stack_file_state *state);
int np_session_stack_finish_file(struct np_session_stack_file_state *state);

struct np_session_stack_operations {
    int (*stage)(void *context, const struct np_session_stack_artifact *artifact,
                 size_t index);
    int (*commit)(void *context, const struct np_session_stack_artifact *artifact,
                  size_t index);
    int (*rollback)(void *context,
                    const struct np_session_stack_artifact *artifact,
                    size_t index);
    void (*finish)(void *context,
                   const struct np_session_stack_artifact *artifact,
                   size_t index);
    void (*discard)(void *context, const struct np_session_stack_artifact *artifact,
                    size_t index);
    int (*run)(void *context, const char *const arguments[]);
    void *context;
};

/*
 * Stage every artifact before changing any destination or service state.
 * Commit callbacks atomically replace one destination after all stages exist,
 * retaining the old target until finish. A failed commit rolls previously
 * committed targets back in reverse order and runs no service command. Once
 * every commit succeeds, the selected supervisor is reloaded and restarted.
 * Only then does finish discard backups. A supervisor failure rolls every
 * target back in reverse order and reloads/restarts the restored service.
 */
int np_session_stack_apply(
    const struct np_session_stack_artifact *artifacts,
    size_t artifact_count,
    enum np_session_stack_service service,
    const struct np_session_stack_operations *operations);

#endif
