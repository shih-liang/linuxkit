#define _POSIX_C_SOURCE 200809L

#include "session_stack.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fake_operations {
    int fail_install_at;
    int fail_commit_at;
    int fail_run_at;
    int install_attempts;
    int commit_attempts;
    int rollback_attempts;
    int finish_attempts;
    int discard_attempts;
    int run_attempts;
    int run_attempts_at_first_finish;
    int restart_attempts;
    int target_generation[4];
    int rollback_order[8];
    char commands[8][128];
};

static void fail(const char *message) {
    fprintf(stderr, "session_stack_test: %s\n", message);
    exit(1);
}

static void check(int condition, const char *message) {
    if (!condition)
        fail(message);
}

static void write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "could not create transaction fixture");
    size_t length = strlen(text);
    check(write(fd, text, length) == (ssize_t)length,
          "could not write transaction fixture");
    check(close(fd) == 0, "could not close transaction fixture");
}

static void check_text(const char *path, const char *expected,
                       const char *message) {
    char buffer[32];
    int fd = open(path, O_RDONLY);
    check(fd >= 0, message);
    ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    check(length >= 0, message);
    buffer[length] = '\0';
    check(close(fd) == 0, message);
    check(strcmp(buffer, expected) == 0, message);
}

static int fake_stage(void *context,
                      const struct np_session_stack_artifact *artifact,
                      size_t index) {
    struct fake_operations *fake = context;
    int attempt = fake->install_attempts++;
    check((int)index == attempt, "stage index is wrong");
    check(artifact->name != NULL, "missing artifact name");
    check(artifact->destination != NULL, "missing artifact destination");
    check(artifact->mode == 0644 || artifact->mode == 0755,
          "invalid artifact mode");
    return attempt == fake->fail_install_at ? -1 : 0;
}

static int fake_commit(void *context,
                       const struct np_session_stack_artifact *artifact,
                       size_t index) {
    struct fake_operations *fake = context;
    int attempt = fake->commit_attempts++;
    check((int)index == attempt, "commit index is wrong");
    check(artifact->destination != NULL, "commit destination is missing");
    if (attempt == fake->fail_commit_at)
        return -1;
    fake->target_generation[index] = 1;
    return 0;
}

static int fake_rollback(void *context,
                         const struct np_session_stack_artifact *artifact,
                         size_t index) {
    struct fake_operations *fake = context;
    check(artifact->destination != NULL, "rollback destination is missing");
    if (fake->target_generation[index] == 0)
        return 0;
    fake->target_generation[index] = 0;
    fake->rollback_order[fake->rollback_attempts++] = (int)index;
    return 0;
}

static void fake_finish(void *context,
                        const struct np_session_stack_artifact *artifact,
                        size_t index) {
    struct fake_operations *fake = context;
    check(artifact->destination != NULL, "finish destination is missing");
    check(fake->target_generation[index] == 1,
          "finish ran before the new target was committed");
    if (fake->finish_attempts == 0)
        fake->run_attempts_at_first_finish = fake->run_attempts;
    fake->finish_attempts++;
}

static void fake_discard(void *context,
                         const struct np_session_stack_artifact *artifact,
                         size_t index) {
    struct fake_operations *fake = context;
    (void)index;
    check(artifact->destination != NULL, "discard destination is missing");
    fake->discard_attempts++;
}

static int fake_run(void *context, const char *const arguments[]) {
    struct fake_operations *fake = context;
    int attempt = fake->run_attempts++;
    size_t used = 0;
    int is_restart = 0;
    for (size_t index = 0; arguments[index] != NULL; index++) {
        if (strcmp(arguments[index], "restart") == 0)
            is_restart = 1;
        int written = snprintf(fake->commands[attempt] + used,
                               sizeof(fake->commands[attempt]) - used,
                               "%s%s", index == 0 ? "" : " ", arguments[index]);
        check(written >= 0 && (size_t)written < sizeof(fake->commands[attempt]) - used,
              "command fixture overflow");
        used += (size_t)written;
    }
    if (is_restart)
        fake->restart_attempts++;
    return attempt == fake->fail_run_at ? -1 : 0;
}

static const struct np_session_stack_artifact artifacts[] = {
    {"session", "/session", 0755},
    {"compositor", "/compositor", 0755},
    {"profile", "/profile", 0644},
    {"unit", "/unit", 0644},
};

static int apply(enum np_session_stack_service service,
                 struct fake_operations *fake) {
    struct np_session_stack_operations operations = {
        .stage = fake_stage,
        .commit = fake_commit,
        .rollback = fake_rollback,
        .finish = fake_finish,
        .discard = fake_discard,
        .run = fake_run,
        .context = fake,
    };
    return np_session_stack_apply(
        artifacts, sizeof(artifacts) / sizeof(artifacts[0]), service, &operations);
}

static void check_target_generation(const struct fake_operations *fake,
                                    int expected, const char *message) {
    for (size_t index = 0; index < sizeof(fake->target_generation) /
                                      sizeof(fake->target_generation[0]); index++)
        check(fake->target_generation[index] == expected, message);
}

static void test_partial_pull_does_not_restart(void) {
    struct fake_operations fake = {
        .fail_install_at = 2, .fail_commit_at = -1, .fail_run_at = -1,
    };
    check(apply(NP_SESSION_STACK_SYSTEMD, &fake) < 0,
          "partial artifact failure was accepted");
    check(fake.install_attempts == 3, "installer continued after artifact failure");
    check(fake.commit_attempts == 0, "destination changed after artifact failure");
    check(fake.discard_attempts == 2, "staged artifacts were not discarded");
    check(fake.run_attempts == 0, "service command ran after artifact failure");
    check(fake.restart_attempts == 0, "service restarted after artifact failure");
    check_target_generation(&fake, 0, "partial pull changed a target");
}

static void test_invalid_service_changes_nothing(void) {
    struct fake_operations fake = {
        .fail_install_at = -1, .fail_commit_at = -1, .fail_run_at = -1,
    };
    check(apply((enum np_session_stack_service)99, &fake) < 0,
          "invalid supervisor was accepted");
    check(fake.install_attempts == 0, "invalid supervisor staged artifacts");
    check(fake.commit_attempts == 0, "invalid supervisor changed destinations");
    check(fake.run_attempts == 0, "invalid supervisor ran a command");
    check_target_generation(&fake, 0, "invalid supervisor changed a target");
}

static void test_systemd_restarts_exactly_once(void) {
    struct fake_operations fake = {
        .fail_install_at = -1, .fail_commit_at = -1, .fail_run_at = -1,
    };
    check(apply(NP_SESSION_STACK_SYSTEMD, &fake) == 0,
          "complete systemd transaction failed");
    check(fake.install_attempts == 4, "not every systemd artifact was installed");
    check(fake.commit_attempts == 4, "not every systemd artifact was committed");
    check(fake.run_attempts == 3, "wrong systemd command count");
    check(fake.restart_attempts == 1, "systemd did not restart exactly once");
    check(fake.rollback_attempts == 0, "successful systemd install rolled back");
    check(fake.finish_attempts == 4, "systemd backups were not finalized");
    check(fake.run_attempts_at_first_finish == 3,
          "systemd backups were finalized before service success");
    check_target_generation(&fake, 1, "systemd target is not the new generation");
    check(strcmp(fake.commands[0], "systemctl daemon-reload") == 0,
          "systemd did not reload units first");
    check(strcmp(fake.commands[1], "systemctl enable nativepipe-session.service") == 0,
          "systemd enable command is wrong");
    check(strcmp(fake.commands[2], "systemctl restart nativepipe-session.service") == 0,
          "systemd restart command is wrong");
}

static void test_commit_failure_does_not_restart(void) {
    struct fake_operations fake = {
        .fail_install_at = -1, .fail_commit_at = 2, .fail_run_at = -1,
    };
    check(apply(NP_SESSION_STACK_SYSTEMD, &fake) < 0,
          "failed atomic replacement was accepted");
    check(fake.install_attempts == 4, "not every artifact was staged before commit");
    check(fake.commit_attempts == 3, "commit continued after replacement failure");
    check(fake.discard_attempts == 2, "pending staged artifacts were not discarded");
    check(fake.rollback_attempts == 2, "committed targets were not rolled back");
    check(fake.rollback_order[0] == 1 && fake.rollback_order[1] == 0,
          "committed targets were not rolled back in reverse order");
    check(fake.finish_attempts == 0, "rollback discarded the old backups");
    check(fake.run_attempts == 0, "service command ran after replacement failure");
    check(fake.restart_attempts == 0, "service restarted after replacement failure");
    check_target_generation(&fake, 0, "commit failure left a mixed target generation");
}

static void test_openrc_restarts_exactly_once(void) {
    struct fake_operations fake = {
        .fail_install_at = -1, .fail_commit_at = -1, .fail_run_at = -1,
    };
    check(apply(NP_SESSION_STACK_OPENRC, &fake) == 0,
          "complete OpenRC transaction failed");
    check(fake.install_attempts == 4, "not every OpenRC artifact was installed");
    check(fake.commit_attempts == 4, "not every OpenRC artifact was committed");
    check(fake.run_attempts == 2, "wrong OpenRC command count");
    check(fake.restart_attempts == 1, "OpenRC did not restart exactly once");
    check(fake.rollback_attempts == 0, "successful OpenRC install rolled back");
    check(fake.finish_attempts == 4, "OpenRC backups were not finalized");
    check(fake.run_attempts_at_first_finish == 2,
          "OpenRC backups were finalized before service success");
    check_target_generation(&fake, 1, "OpenRC target is not the new generation");
    check(strcmp(fake.commands[0],
                 "rc-update add nativepipe-session default") == 0,
          "OpenRC enable command is wrong");
    check(strcmp(fake.commands[1],
                 "rc-service nativepipe-session restart") == 0,
          "OpenRC restart command is wrong");
}

static void test_enable_failure_does_not_restart(void) {
    struct fake_operations fake = {
        .fail_install_at = -1, .fail_commit_at = -1, .fail_run_at = 1,
    };
    check(apply(NP_SESSION_STACK_SYSTEMD, &fake) < 0,
          "failed systemd enable was accepted");
    check(fake.run_attempts == 4,
          "systemd recovery did not reload and restart the restored unit");
    check(fake.restart_attempts == 1,
          "restored systemd service was not restarted exactly once");
    check(fake.rollback_attempts == 4,
          "service failure did not restore every old target");
    check(fake.rollback_order[0] == 3 && fake.rollback_order[1] == 2 &&
          fake.rollback_order[2] == 1 && fake.rollback_order[3] == 0,
          "service failure did not roll targets back in reverse order");
    check(fake.finish_attempts == 0,
          "service failure finalized backups before recovery");
    check_target_generation(&fake, 0,
                            "service failure retained the new generation");
    check(strcmp(fake.commands[2], "systemctl daemon-reload") == 0,
          "systemd recovery did not reload the restored unit");
    check(strcmp(fake.commands[3],
                 "systemctl restart nativepipe-session.service") == 0,
          "systemd recovery did not restart the restored service");
}

static void test_restart_failure_restores_previous_generation(void) {
    struct fake_operations fake = {
        .fail_install_at = -1, .fail_commit_at = -1, .fail_run_at = 2,
    };
    check(apply(NP_SESSION_STACK_SYSTEMD, &fake) < 0,
          "failed systemd restart was accepted");
    check(fake.run_attempts == 5,
          "systemd restart failure did not run the recovery sequence");
    check(fake.restart_attempts == 2,
          "systemd recovery did not restart the restored service");
    check(fake.rollback_attempts == 4,
          "restart failure did not restore every old target");
    check(fake.finish_attempts == 0,
          "restart failure finalized backups before recovery");
    check_target_generation(&fake, 0,
                            "restart failure retained the new generation");
    check(strcmp(fake.commands[3], "systemctl daemon-reload") == 0,
          "restart recovery did not reload the restored unit");
    check(strcmp(fake.commands[4],
                 "systemctl restart nativepipe-session.service") == 0,
          "restart recovery did not restart the restored service");
}

static void test_openrc_restart_failure_restores_previous_generation(void) {
    struct fake_operations fake = {
        .fail_install_at = -1, .fail_commit_at = -1, .fail_run_at = 1,
    };
    check(apply(NP_SESSION_STACK_OPENRC, &fake) < 0,
          "failed OpenRC restart was accepted");
    check(fake.run_attempts == 3,
          "OpenRC restart failure did not restart the restored service");
    check(fake.restart_attempts == 2,
          "OpenRC recovery restart count is wrong");
    check(fake.rollback_attempts == 4,
          "OpenRC restart failure did not restore every old target");
    check(fake.finish_attempts == 0,
          "OpenRC restart failure finalized backups before recovery");
    check_target_generation(&fake, 0,
                            "OpenRC restart failure retained the new generation");
    check(strcmp(fake.commands[2],
                 "rc-service nativepipe-session restart") == 0,
          "OpenRC recovery did not restart the restored service");
}

static void test_file_backup_rollback_and_finish(void) {
    char directory[] = "/tmp/nativepipe-session-stack.XXXXXX";
    int directory_fixture = mkstemp(directory);
    check(directory_fixture >= 0, "could not reserve transaction directory");
    check(close(directory_fixture) == 0 && unlink(directory) == 0 &&
          mkdir(directory, 0700) == 0,
          "could not create transaction directory");
    char target[256], staging[256], backup[256];
    check(snprintf(target, sizeof(target), "%s/target", directory) > 0,
          "could not form target path");
    check(snprintf(staging, sizeof(staging), "%s/staging", directory) > 0,
          "could not form staging path");
    check(snprintf(backup, sizeof(backup), "%s/backup", directory) > 0,
          "could not form backup path");
    const struct np_session_stack_artifact artifact = {
        "fixture", target, 0644,
    };
    struct np_session_stack_file_state state = {0};

    write_text(target, "old");
    write_text(staging, "new");
    check(np_session_stack_commit_file(&artifact, staging, backup, &state) == 0,
          "file transaction commit failed");
    check_text(target, "new", "commit did not publish the new target");
    check_text(backup, "old", "commit did not retain the old target");
    check(np_session_stack_rollback_file(&artifact, &state) == 0,
          "file transaction rollback failed");
    check_text(target, "old", "rollback did not restore the old target");
    check(access(backup, F_OK) < 0 && errno == ENOENT,
          "rollback retained the old-target backup");

    write_text(staging, "newer");
    check(np_session_stack_commit_file(&artifact, staging, backup, &state) == 0,
          "second file transaction commit failed");
    check(np_session_stack_finish_file(&state) == 0,
          "file transaction finalization failed");
    check_text(target, "newer", "finalization changed the new target");
    check(access(backup, F_OK) < 0 && errno == ENOENT,
          "finalization retained the old-target backup");
    check(unlink(target) == 0 && rmdir(directory) == 0,
          "could not remove transaction fixtures");
}

int main(void) {
    test_partial_pull_does_not_restart();
    test_invalid_service_changes_nothing();
    test_commit_failure_does_not_restart();
    test_systemd_restarts_exactly_once();
    test_openrc_restarts_exactly_once();
    test_enable_failure_does_not_restart();
    test_restart_failure_restores_previous_generation();
    test_openrc_restart_failure_restores_previous_generation();
    test_file_backup_rollback_and_finish();
    puts("session_stack_test: ok");
    return 0;
}
