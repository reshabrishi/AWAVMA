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
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    awavma_runtime_record_t records[4];
    pid_t child;
    size_t count;
    int passed = 1;

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
    passed = runtime != NULL && awavma_runtime_init(runtime, &config) == 0;
    report("AR01", passed);
    if (passed)
        passed = awavma_runtime_run_for(runtime, 220) == 0;
    report("AR02", passed);
    count = runtime == NULL ? 0 : awavma_runtime_snapshot(runtime, records, 4);
    passed = passed && count == 1 && records[0].pid == child && records[0].start_time_ticks != 0;
    report("AR03", passed);
    passed = passed && records[0].phase3_samples > 0;
    report("AR04", passed);
    passed = passed && records[0].status == AWAVMA_RUNTIME_INSUFFICIENT;
    report("AR05", passed);
    passed = passed && strstr(records[0].detail, "unavailable") != NULL;
    report("AR06", passed);
    snprintf(results_path, sizeof(results_path), "%s/runtime_results.csv", root);
    passed = passed && access(results_path, R_OK) == 0;
    report("AR07", passed);
    awavma_runtime_destroy(runtime);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
