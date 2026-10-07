#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"

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

static bool target_filter(const application_manager_record_t *application, void *context)
{
    return application->pid == *(const pid_t *)context;
}

int main(void)
{
    char root[] = "/tmp/awavma-runtime-test-XXXXXX";
    char cwd[PATH_MAX];
    char bin_dir[PATH_MAX + 32];
    char config_path[PATH_MAX + 32];
    char results_path[PATH_MAX + 32];
    char history_path[PATH_MAX + 256];
    char history[2048] = {0};
    char failure_root[] = "/tmp/awavma-temporal-failure-XXXXXX";
    char missing_bin[] = "/tmp/awavma-no-classifier";
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    awavma_runtime_record_t records[4];
    pid_t child;
    size_t count;
    int passed = 1;
    int ar01, ar02, ar03, ar04, ar05, ar06, ar07, ar08, ar09;

    if (mkdtemp(root) == NULL || getcwd(cwd, sizeof(cwd)) == NULL) {
        report("AR01", 0);
        return EXIT_FAILURE;
    }
    child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    if (child < 0) {
        report("AR01", 0);
        return EXIT_FAILURE;
    }
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cwd);
    snprintf(config_path, sizeof(config_path), "%s/config/awavma.conf", cwd);
    awavma_runtime_config_default(&config);
    config.root_dir = root;
    config.bin_dir = bin_dir;
    config.phase_config_path = config_path;
    config.monitor_interval_ms = 25;
    config.evaluation_interval_ms = 160;
    config.max_applications = 256;
    config.worker_count = 1;
    config.queue_capacity = 4;
    config.application_filter = target_filter;
    config.application_filter_context = &child;
    runtime = awavma_runtime_create();
    ar01 = runtime != NULL && awavma_runtime_init(runtime, &config) == 0;
    report("AR01", ar01);
    ar02 = ar01 && awavma_runtime_run_for(runtime, 220) == 0;
    report("AR02", ar02);
    count = runtime == NULL ? 0 : awavma_runtime_snapshot(runtime, records, 4);
    ar03 = ar02 && count == 1 && records[0].pid == child && records[0].start_time_ticks != 0;
    report("AR03", ar03);
    ar04 = ar03 && records[0].phase3_samples > 0;
    report("AR04", ar04);
    ar05 = ar03 && records[0].status == AWAVMA_RUNTIME_INSUFFICIENT;
    report("AR05", ar05);
    ar06 = ar03 && strstr(records[0].detail, "unavailable") != NULL;
    report("AR06", ar06);
    snprintf(results_path, sizeof(results_path), "%s/runtime_results.csv", root);
    ar07 = ar02 && access(results_path, R_OK) == 0;
    report("AR07", ar07);
    snprintf(history_path, sizeof(history_path), "%s/apps/%s/history/thread_confidence.csv", root,
             records[0].app_id);
    FILE *history_file = fopen(history_path, "r");
    ar08 = ar03 && records[0].temporal_generation >= 1 && records[0].temporal_history_available &&
              history_file != NULL && fread(history, 1, sizeof(history) - 1, history_file) > 0 &&
              fclose(history_file) == 0 && strstr(history, ",NA,false,false,") != NULL &&
              strstr(history, "0x") == NULL && strstr(history, "address") == NULL;
    report("AR08", ar08);
    awavma_runtime_destroy(runtime);
    runtime = NULL;
    passed = passed && mkdtemp(failure_root) != NULL;
    awavma_runtime_config_default(&config);
    config.root_dir = failure_root;
    config.bin_dir = missing_bin;
    config.phase_config_path = config_path;
    config.phase4_mode = AWAVMA_PHASE4_SUBPROCESS;
    config.monitor_interval_ms = 25;
    config.evaluation_interval_ms = 160;
    config.max_applications = 256;
    config.worker_count = 1;
    config.queue_capacity = 4;
    config.application_filter = target_filter;
    config.application_filter_context = &child;
    runtime = awavma_runtime_create();
    int failure_run = runtime != NULL && awavma_runtime_init(runtime, &config) == 0 ?
        awavma_runtime_run_for(runtime, 220) : 0;
    count = runtime == NULL ? 0 : awavma_runtime_snapshot(runtime, records, 4);
    ar09 = failure_run == 0 && count == 1 && records[0].generation == 0 &&
           records[0].temporal_generation >= 1 && records[0].temporal_history_available;
    report("AR09", ar09);
    awavma_runtime_destroy(runtime);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    passed = ar01 && ar02 && ar03 && ar04 && ar05 && ar06 && ar07 && ar08 && ar09;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
