#define _POSIX_C_SOURCE 200809L

#include "migration_safety_manager.h"
#include "runtime_migration_metadata.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { FIXTURE_NO_ALTERNATE, FIXTURE_UNAVAILABLE, FIXTURE_INVALID, FIXTURE_STALE } fixture_kind_t;
typedef struct { fixture_kind_t kind; unsigned target; unsigned execute; unsigned rollback; unsigned feedback; FeedbackEvent event; } fixture_t;

static uint64_t ticks_for(pid_t pid)
{
    char path[64], line[4096], *cursor, *save = NULL;
    FILE *file;
    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) fclose(file);
        return 0;
    }
    fclose(file); cursor = strrchr(line, ')');
    if (cursor == NULL) return 0;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor + 2 : NULL, " ", &save);
        if (token == NULL) return 0;
        if (field == 22) return strtoull(token, NULL, 10);
    }
    return 0;
}

static MigrationTargetResult production_target(void *context, const MigrationSafetyRequest *request,
                                                const char *attempt, MigrationTarget *target)
{
    fixture_t *fixture = context;
    MigrationTargetInput input = {0};

    fixture->target++;
    input.pid = request->pid; input.start_time_ticks = request->start_time_ticks;
    input.attempt_id = fixture->kind == FIXTURE_STALE ? "other-attempt" : attempt;
    input.action = request->action; input.source_node_available = true; input.source_numa_node = 0;
    input.source = MIGRATION_TARGET_SOURCE_PHASE5;
    if (fixture->kind == FIXTURE_NO_ALTERNATE) input.requires_cross_node = true;
    if (fixture->kind == FIXTURE_INVALID) {
        input.has_authoritative_cpu_mask = true;
        CPU_SET(CPU_SETSIZE - 1, &input.authoritative_cpu_mask);
    }
    if (fixture->kind == FIXTURE_STALE) {
        input.has_authoritative_cpu_mask = true;
        CPU_SET(0, &input.authoritative_cpu_mask);
    }
    return migration_target_provider_get(&input, NULL, target);
}

static MigrationResultCode executor(void *context, const MigrationRequest *request, MigrationReport *report)
{ (void)request; (void)report; ((fixture_t *)context)->execute++; return MIGRATION_SYSTEM_ERROR; }
static MigrationSafetyRecovery rollback(void *context, const MigrationSafetyRequest *request, const MigrationReport *report)
{ (void)request; (void)report; ((fixture_t *)context)->rollback++; return MIGRATION_SAFETY_ROLLBACK_FAILED_RESULT; }
static bool feedback(void *context, const FeedbackEvent *event, FeedbackResult *result)
{ fixture_t *fixture = context; fixture->feedback++; fixture->event = *event; (void)result; return true; }

static int run_case(pid_t pid, uint64_t ticks, fixture_kind_t kind, FeedbackTerminalOutcome expected)
{
    MigrationSafetyConfig config;
    MigrationSafetyRequest request = {0};
    MigrationSafetyResult result;
    MigrationSafetyManager *manager = migration_safety_manager_create();
    RuntimeMigrationMetadata metadata;
    RuntimeMigrationCheckpoint checkpoint;
    fixture_t fixture = {.kind = kind};
    int passed;

    if (!runtime_get_migration_metadata(pid, ticks, &metadata) ||
        !runtime_migration_checkpoint_affinity(&metadata, &checkpoint)) return 0;
    migration_safety_config_default(&config); config.enabled = true; config.execution_enabled = false;
    config.target_fn = production_target; config.execute_fn = executor; config.rollback_fn = rollback;
    config.feedback_fn = feedback; config.callback_context = &fixture;
    if (manager == NULL || !migration_safety_manager_init(manager, &config)) return 0;
    snprintf(request.app_id, sizeof(request.app_id), "target-integration"); request.pid = pid;
    request.start_time_ticks = ticks; request.action = kind == FIXTURE_NO_ALTERNATE ? VALIDATION_ACTION_MOVE_MEMORY : VALIDATION_ACTION_MOVE_THREAD;
    request.migration_request.pid = pid; request.migration_request.start_time_ticks = ticks;
    request.migration_request.start_time_ticks_available = true; request.migration_request.phase5_decision.action = request.action;
    snprintf(request.migration_request.phase6_validation.final_decision, sizeof(request.migration_request.phase6_validation.final_decision), "APPROVED");
    passed = migration_safety_manager_attempt(manager, &request, &result) && fixture.target == 1 &&
             fixture.execute == 0 && fixture.rollback == 0 && fixture.feedback == 1 &&
             fixture.event.terminal_outcome == expected && fixture.event.pid == pid &&
             fixture.event.start_time_ticks == ticks && strcmp(fixture.event.migration_id, result.attempt_id) == 0;
    runtime_migration_checkpoint_release(&checkpoint); migration_safety_manager_destroy(manager);
    return passed;
}

int main(void)
{
    pid_t child = fork(); uint64_t ticks; int passed;
    if (child == 0) for (;;) pause();
    if (child < 0) return EXIT_FAILURE;
    usleep(10000); ticks = ticks_for(child);
    passed = ticks != 0 && run_case(child, ticks, FIXTURE_NO_ALTERNATE, FEEDBACK_TERMINAL_NO_ALTERNATE_TARGET);
    printf("RTI01_NO_ALTERNATE: %s\n", passed ? "PASS" : "FAIL");
    passed = passed && run_case(child, ticks, FIXTURE_UNAVAILABLE, FEEDBACK_TERMINAL_TARGET_UNAVAILABLE);
    printf("RTI02_UNAVAILABLE: %s\n", passed ? "PASS" : "FAIL");
    passed = passed && run_case(child, ticks, FIXTURE_INVALID, FEEDBACK_TERMINAL_TARGET_INVALID);
    printf("RTI03_INVALID: %s\n", passed ? "PASS" : "FAIL");
    kill(child, SIGTERM); waitpid(child, NULL, 0);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
