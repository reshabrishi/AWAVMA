#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"
#include "runtime_target_filter.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void report(const char *id, int passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
}

static int file_contains(const char *path, const char *text)
{
    FILE *file = fopen(path, "r");
    char buffer[4096] = {0};
    size_t length;

    if (file == NULL)
        return 0;
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';
    return strstr(buffer, text) != NULL;
}

int main(void)
{
    char root[] = "/tmp/awavma-runtime-test-XXXXXX";
    char cwd[PATH_MAX];
    char bin_dir[PATH_MAX + 32];
    char config_path[PATH_MAX + 32];
    char results_path[PATH_MAX + 32];
    char classification_path[PATH_MAX + 96];
    char decision_path[PATH_MAX + 96];
    char validation_input_path[PATH_MAX + 96];
    char validation_path[PATH_MAX + 96];
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    awavma_runtime_record_t records[4];
    runtime_target_filter_t target_filter;
    pid_t child;
    size_t count;
    int passed = 1;
    int suite_passed = 1;

    if (mkdtemp(root) == NULL || getcwd(cwd, sizeof(cwd)) == NULL) {
        report("AR01", 0);
        return EXIT_FAILURE;
    }
    runtime_target_filter_init(&target_filter);
    child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    if (child < 0) {
        runtime_target_filter_cleanup(&target_filter);
        report("AR01", 0);
        return EXIT_FAILURE;
    }
    if (runtime_target_filter_add_pid(&target_filter, child) != 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        runtime_target_filter_cleanup(&target_filter);
        report("AR01", 0);
        return EXIT_FAILURE;
    }
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cwd);
    snprintf(config_path, sizeof(config_path), "%s/config/awavma.conf", cwd);
    awavma_runtime_config_default(&config);
    passed = config.benefit_calibration_state == BENEFIT_CALIBRATION_UNAVAILABLE;
    report("AR00_DEFAULT_CALIBRATION_UNAVAILABLE", passed);
    suite_passed = suite_passed && passed;
    config.root_dir = root;
    config.bin_dir = bin_dir;
    config.phase_config_path = config_path;
    config.monitor_interval_ms = 25;
    config.evaluation_interval_ms = 160;
    config.max_applications = 256;
    config.worker_count = 1;
    config.queue_capacity = 4;
    config.discovery_filter = runtime_target_filter_matches_discovery;
    config.discovery_filter_context = &target_filter;
    config.application_filter = runtime_target_filter_matches;
    config.application_filter_context = &target_filter;
    runtime = awavma_runtime_create();
    passed = runtime != NULL && awavma_runtime_init(runtime, &config) == 0;
    report("AR01", passed);
    suite_passed = suite_passed && passed;
    if (passed)
        passed = awavma_runtime_run_for(runtime, 220) == 0;
    report("AR02", passed);
    suite_passed = suite_passed && passed;
    count = runtime == NULL ? 0 : awavma_runtime_snapshot(runtime, records, 4);
    passed = passed && count == 1 && records[0].pid == child && records[0].start_time_ticks != 0;
    report("AR03", passed);
    suite_passed = suite_passed && passed;
    passed = passed && records[0].phase3_samples > 0;
    report("AR04", passed);
    suite_passed = suite_passed && passed;
    bool ar05 = runtime != NULL && count == 1 && records[0].status == AWAVMA_RUNTIME_REJECTED;
    report("AR05", ar05);
    suite_passed = suite_passed && ar05;
    bool ar06 = false;
    if (runtime != NULL && count == 1) {
        snprintf(classification_path, sizeof(classification_path), "%s/apps/%s/cycles/1/classification_full.csv",
                 root, records[0].app_id);
        snprintf(decision_path, sizeof(decision_path), "%s/apps/%s/cycles/1/decision.csv", root,
                 records[0].app_id);
        snprintf(validation_path, sizeof(validation_path), "%s/apps/%s/cycles/1/validation.csv", root,
                 records[0].app_id);
        snprintf(validation_input_path, sizeof(validation_input_path), "%s/apps/%s/cycles/1/validation_input.csv", root,
                 records[0].app_id);
        ar06 = strcmp(records[0].detail, "Phase 6 did not approve migration") == 0 &&
               access(classification_path, R_OK) == 0 && access(decision_path, R_OK) == 0 &&
                access(validation_path, R_OK) == 0 && file_contains(classification_path, ",COLD,") &&
                file_contains(decision_path, "INSUFFICIENT_DECISION_SIGNAL") &&
                file_contains(validation_input_path, ",NO_MIGRATION,INSUFFICIENT_DECISION_SIGNAL,") &&
                file_contains(validation_path, ",NO_MIGRATION");
    }
    report("AR06", ar06);
    suite_passed = suite_passed && ar06;
    snprintf(results_path, sizeof(results_path), "%s/runtime_results.csv", root);
    bool ar07 = access(results_path, R_OK) == 0;
    report("AR07", ar07);
    suite_passed = suite_passed && ar07;
    awavma_runtime_destroy(runtime);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    runtime_target_filter_cleanup(&target_filter);
    return suite_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
