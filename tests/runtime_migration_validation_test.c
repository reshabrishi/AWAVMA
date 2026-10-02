#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"

#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t start_ticks(pid_t pid)
{
    char path[64];
    char line[4096];
    char *cursor;
    char *save = NULL;
    FILE *file;

    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL)
            fclose(file);
        return 0;
    }
    fclose(file);
    cursor = strrchr(line, ')');
    if (cursor == NULL || cursor[1] != ' ')
        return 0;
    cursor += 2;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor : NULL, " ", &save);

        if (token == NULL)
            return 0;
        if (field == 22)
            return strtoull(token, NULL, 10);
    }
    return 0;
}

static awavma_runtime_t *start_runtime(char *root, bool execution_enabled)
{
    char cwd[PATH_MAX];
    char bin_dir[PATH_MAX + 32];
    char config_path[PATH_MAX + 32];
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;

    if (mkdtemp(root) == NULL || getcwd(cwd, sizeof(cwd)) == NULL)
        return NULL;
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cwd);
    snprintf(config_path, sizeof(config_path), "%s/config/awavma.conf", cwd);
    awavma_runtime_config_default(&config);
    config.root_dir = root;
    config.bin_dir = bin_dir;
    config.phase_config_path = config_path;
    config.migration_safety_enabled = true;
    config.migration_execution_enabled = execution_enabled;
    /* Test-only execution still passes runtime initialization's production gate. */
    config.benefit_calibration_state = BENEFIT_CALIBRATION_VALIDATED_PRODUCTION;
    runtime = awavma_runtime_create();
    if (runtime == NULL || awavma_runtime_init(runtime, &config) != 0) {
        awavma_runtime_destroy(runtime);
        return NULL;
    }
    return runtime;
}

static bool feedback_once(const char *root, const awavma_runtime_test_target_stats_t *stats,
                          const char *structural_fields)
{
    char path[PATH_MAX + 256];
    char line[4096];
    FILE *file;
    unsigned rows = 0;
    bool fields_found = false;

    if (snprintf(path, sizeof(path), "%s/apps/target-integration/history/migration_feedback.csv",
                 root) >= (int)sizeof(path) || (file = fopen(path, "r")) == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, stats->attempt_id) != NULL) {
            rows++;
            fields_found = structural_fields == NULL || strstr(line, structural_fields) != NULL;
        }
    fclose(file);
    return rows == 1 && fields_found;
}

static bool structural_validation_case(pid_t child, uint64_t ticks)
{
    char root[] = "/tmp/awavma-runtime-validation-XXXXXX";
    awavma_runtime_t *runtime;
    awavma_runtime_test_target_stats_t stats;
    cpu_set_t before;
    cpu_set_t after;
    bool have_before;
    bool passed;

    runtime = start_runtime(root, true);
    have_before = runtime != NULL && sched_getaffinity(child, sizeof(before), &before) == 0;
    passed = have_before && CPU_COUNT(&before) > 1 &&
             awavma_runtime_test_submit_approved_migration(
                 runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_VALID, &stats) == 0 &&
              stats.target_provider_calls == 1 && stats.executor_calls == 1 &&
              stats.rollback_calls == 0 && stats.validation_before_calls == 1 &&
              stats.validation_after_calls == 1 && stats.terminal_feedback_calls == 1 &&
              stats.structural_validation_committed && stats.structural_validation_known &&
               stats.structural_validation_succeeded && stats.progress_known &&
               stats.progress_observed && !stats.benefit_known &&
              stats.before_cpu_time_available && stats.after_cpu_time_available &&
              stats.after_cpu_time_ticks >= stats.before_cpu_time_ticks &&
               feedback_once(root, &stats, ",true,true,true,true,false,") &&
              sched_getaffinity(child, sizeof(after), &after) == 0 && CPU_COUNT(&after) == 1;
    if (have_before && sched_setaffinity(child, sizeof(before), &before) != 0)
        passed = false;
    awavma_runtime_destroy(runtime);
    return passed;
}

static bool placement_mismatch_case(pid_t child, uint64_t ticks)
{
    char root[] = "/tmp/awavma-runtime-validation-XXXXXX";
    awavma_runtime_t *runtime;
    awavma_runtime_test_target_stats_t stats;
    cpu_set_t before;
    cpu_set_t after;
    bool passed;

    runtime = start_runtime(root, true);
    passed = runtime != NULL && sched_getaffinity(child, sizeof(before), &before) == 0 &&
             CPU_COUNT(&before) > 1 && awavma_runtime_test_submit_approved_migration(
                 runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_PLACEMENT_MISMATCH, &stats) == 0 &&
             stats.target_provider_calls == 1 && stats.executor_calls == 1 &&
             stats.validation_before_calls == 1 && stats.validation_after_calls == 1 &&
             stats.rollback_calls == 1 && stats.rollback_succeeded &&
             stats.terminal_feedback_calls == 1 && stats.structural_validation_known &&
             !stats.structural_validation_succeeded && !stats.progress_known &&
             !stats.progress_observed && !stats.benefit_known &&
             feedback_once(root, &stats, ",true,false,false,false,false,") &&
             sched_getaffinity(child, sizeof(after), &after) == 0 && CPU_EQUAL(&before, &after);
    awavma_runtime_destroy(runtime);
    return passed;
}

static bool executor_verification_failure_case(pid_t child, uint64_t ticks)
{
    char root[] = "/tmp/awavma-runtime-execution-verification-XXXXXX";
    awavma_runtime_t *runtime;
    awavma_runtime_test_target_stats_t stats = {0};
    cpu_set_t before, after;
    bool have_before = false, have_after = false, cpu_equal = false;
    int submit_result = -1, before_count = 0, after_count = 0;
    bool passed;

    runtime = start_runtime(root, true);
    if (runtime != NULL && sched_getaffinity(child, sizeof(before), &before) == 0) {
        have_before = true;
        before_count = CPU_COUNT(&before);
        if (before_count > 1)
            submit_result = awavma_runtime_test_submit_approved_migration(
                runtime, child, ticks,
                AWAVMA_RUNTIME_TEST_TARGET_EXECUTION_VERIFICATION_FAILURE, &stats);
    }
    if (have_before && sched_getaffinity(child, sizeof(after), &after) == 0) {
        have_after = true;
        after_count = CPU_COUNT(&after);
        cpu_equal = CPU_EQUAL(&before, &after);
    }
    printf("RV03B_DIAGNOSTIC submit_return=%d executor_calls=%u validation_before_calls=%u "
           "validation_after_calls=%u rollback_calls=%u rollback_succeeded=%s "
           "execution_result=%d(%s) recovery=%d terminal_state=%d terminal_feedback_calls=%u "
           "CPU_EQUAL=%s CPU_COUNT_before=%d CPU_COUNT_after=%d\n",
           submit_result, stats.executor_calls, stats.validation_before_calls,
           stats.validation_after_calls, stats.rollback_calls,
           stats.rollback_succeeded ? "true" : "false", stats.execution_result,
           MigrationResultName(stats.execution_result), stats.recovery, stats.terminal_state,
           stats.terminal_feedback_calls, cpu_equal ? "true" : "false", before_count, after_count);
    passed = runtime != NULL && have_before && before_count > 1 && submit_result == 0 &&
             stats.executor_calls == 1 && stats.validation_before_calls == 1 &&
             stats.validation_after_calls == 0 && stats.rollback_calls == 1 &&
             stats.rollback_succeeded &&
             stats.execution_result == MIGRATION_VERIFICATION_UNAVAILABLE &&
             stats.recovery == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT &&
             stats.terminal_state == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED &&
             stats.terminal_feedback_calls == 1 && have_after && cpu_equal;
    awavma_runtime_destroy(runtime);
    return passed;
}

static bool idle_validation_case(pid_t child, uint64_t ticks)
{
    char root[] = "/tmp/awavma-runtime-validation-XXXXXX";
    awavma_runtime_t *runtime = start_runtime(root, true);
    awavma_runtime_test_target_stats_t stats;
    cpu_set_t before, after;
    bool passed = runtime != NULL && sched_getaffinity(child, sizeof(before), &before) == 0 &&
        CPU_COUNT(&before) > 1 && awavma_runtime_test_submit_approved_migration(
            runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_VALID, &stats) == 0 &&
        stats.executor_calls == 1 && stats.validation_before_calls == 1 &&
        stats.validation_after_calls == 1 && stats.rollback_calls == 0 &&
        stats.terminal_feedback_calls == 1 && stats.structural_validation_known &&
        stats.structural_validation_succeeded && stats.progress_known &&
        !stats.progress_observed && !stats.benefit_known &&
        stats.before_cpu_time_available && stats.after_cpu_time_available &&
        stats.after_cpu_time_ticks == stats.before_cpu_time_ticks &&
        feedback_once(root, &stats, ",true,true,true,false,false,") &&
        sched_getaffinity(child, sizeof(after), &after) == 0 && CPU_COUNT(&after) == 1;
    if (runtime != NULL && sched_setaffinity(child, sizeof(before), &before) != 0)
        passed = false;
    awavma_runtime_destroy(runtime);
    return passed;
}

static bool capture_failure_case(pid_t child, uint64_t ticks)
{
    char root[] = "/tmp/awavma-runtime-validation-XXXXXX";
    awavma_runtime_t *runtime = start_runtime(root, true);
    awavma_runtime_test_target_stats_t stats;
    bool passed = runtime != NULL && awavma_runtime_test_submit_approved_migration(
        runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_CAPTURE_FAILURE, &stats) == 0 &&
        stats.target_provider_calls == 1 && stats.executor_calls == 0 && stats.rollback_calls == 0 &&
        stats.validation_before_calls == 1 && stats.validation_after_calls == 0 &&
        stats.terminal_feedback_calls == 1 && !stats.structural_validation_known &&
        !stats.progress_known && !stats.progress_observed && !stats.benefit_known &&
        feedback_once(root, &stats, ",false,false,false,false,false,");

    awavma_runtime_destroy(runtime);
    return passed;
}

static bool identity_mismatch_after_exec_case(pid_t child, uint64_t ticks)
{
    char root[] = "/tmp/awavma-runtime-validation-XXXXXX";
    awavma_runtime_t *runtime = start_runtime(root, true);
    awavma_runtime_test_target_stats_t stats;
    bool passed = runtime != NULL && awavma_runtime_test_submit_approved_migration(
        runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_IDENTITY_MISMATCH_AFTER_EXEC, &stats) == 0 &&
        stats.target_provider_calls == 1 && stats.executor_calls == 1 && stats.rollback_calls == 0 &&
        stats.validation_before_calls == 1 && stats.validation_after_calls == 0 &&
        stats.terminal_feedback_calls == 1 && !stats.structural_validation_known &&
        !stats.progress_known && !stats.progress_observed && !stats.benefit_known &&
        feedback_once(root, &stats, ",false,false,false,false,false,");

    awavma_runtime_destroy(runtime);
    return passed;
}

static bool preterminal_case(pid_t child, uint64_t ticks,
                             awavma_runtime_test_target_case_t target_case,
                             bool execution_enabled)
{
    char root[] = "/tmp/awavma-runtime-validation-XXXXXX";
    awavma_runtime_t *runtime = start_runtime(root, execution_enabled);
    awavma_runtime_test_target_stats_t stats;
    bool passed = runtime != NULL && awavma_runtime_test_submit_approved_migration(
        runtime, child, ticks, target_case, &stats) == 0 && stats.target_provider_calls == 1 &&
        stats.executor_calls == 0 && stats.rollback_calls == 0 &&
        stats.validation_before_calls == 0 && stats.validation_after_calls == 0 &&
        stats.terminal_feedback_calls == 1 && !stats.structural_validation_committed;

    awavma_runtime_destroy(runtime);
    return passed;
}

int main(void)
{
    awavma_runtime_config_t defaults;
    pid_t child;
    int ready[2], gate[2];
    uint64_t ticks;
    bool passed, case_pass;

    awavma_runtime_config_default(&defaults);
    passed = !defaults.migration_execution_enabled;
    printf("RV01_EXECUTION_DEFAULT_DISABLED: %s\n", passed ? "PASS" : "FAIL");
    child = fork();
    if (child == 0) {
        for (;;)
            (void)getpid();
    }
    if (child < 0)
        return EXIT_FAILURE;
    usleep(10000);
    ticks = start_ticks(child);
    case_pass = ticks != 0 && structural_validation_case(child, ticks);
    printf("RV02_ACTIVE_CHILD_CPU_SNAPSHOT_STRUCTURAL: %s\n", case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    case_pass = ticks != 0 && placement_mismatch_case(child, ticks);
    printf("RV03_PLACEMENT_MISMATCH_REAL_ROLLBACK: %s\n", case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    case_pass = ticks != 0 && executor_verification_failure_case(child, ticks);
    printf("RV03B_POST_MUTATION_EXECUTOR_VERIFICATION_ROLLBACK: %s\n", case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    case_pass = ticks != 0 && identity_mismatch_after_exec_case(child, ticks);
    printf("RV04_IDENTITY_MISMATCH_AFTER_EXEC: %s\n", case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    case_pass = ticks != 0 && capture_failure_case(child, ticks);
    printf("RV05_AFTER_CAPTURE_FAILURE: %s\n", case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    if (pipe(ready) != 0 || pipe(gate) != 0)
        return EXIT_FAILURE;
    child = fork();
    if (child == 0) {
        char value;
        close(ready[0]); close(gate[1]);
        if (write(ready[1], "R", 1) != 1)
            _exit(EXIT_FAILURE);
        if (read(gate[0], &value, 1) != 1)
            _exit(EXIT_FAILURE);
        _exit(EXIT_SUCCESS);
    }
    if (child < 0)
        return EXIT_FAILURE;
    close(ready[1]); close(gate[0]);
    { char value; if (read(ready[0], &value, 1) != 1) return EXIT_FAILURE; }
    ticks = start_ticks(child);
    case_pass = ticks != 0 && idle_validation_case(child, ticks);
    printf("RV06_IDLE_CHILD_STRUCTURAL_NO_PROGRESS_CLAIM: %s\n", case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    case_pass = ticks != 0 && preterminal_case(child, ticks, AWAVMA_RUNTIME_TEST_TARGET_NO_ALTERNATE, true);
    printf("RV07_TARGET_PRETERMINAL_NO_SNAPSHOTS: %s\n", case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    case_pass = ticks != 0 && preterminal_case(child, ticks, AWAVMA_RUNTIME_TEST_TARGET_VALID, false);
    printf("RV08_EXECUTION_DISABLED_PRETERMINAL_NO_SNAPSHOTS: %s\n", case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    if (write(gate[1], "X", 1) != 1)
        passed = false;
    close(gate[1]);
    waitpid(child, NULL, 0);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
