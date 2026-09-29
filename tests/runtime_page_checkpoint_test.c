#define _POSIX_C_SOURCE 200809L
#include "awavma_runtime.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t start_ticks(pid_t pid)
{
    char path[64], line[4096], *cursor, *save = NULL; FILE *file;
    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL)
            fclose(file);
        return 0;
    }
    fclose(file); cursor = strrchr(line, ')'); if (cursor == NULL) return 0;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor + 2 : NULL, " ", &save);
        if (token == NULL) return 0;
        if (field == 22) return strtoull(token, NULL, 10);
    }
    return 0;
}

int main(void)
{
    char root[] = "/tmp/awavma-page-checkpoint-XXXXXX", cwd[PATH_MAX], bin_dir[PATH_MAX + 32];
    char config_path[PATH_MAX + 32], feedback_path[PATH_MAX + 256], line[4096];
    awavma_runtime_config_t config; awavma_runtime_t *runtime; awavma_runtime_test_target_stats_t stats;
    pid_t child; uint64_t ticks; FILE *file; unsigned rows = 0; bool passed;
    if (mkdtemp(root) == NULL || getcwd(cwd, sizeof(cwd)) == NULL) return EXIT_FAILURE;
    child = fork(); if (child == 0) for (;;) pause(); if (child < 0) return EXIT_FAILURE;
    usleep(10000); ticks = start_ticks(child);
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cwd);
    snprintf(config_path, sizeof(config_path), "%s/config/awavma.conf", cwd);
    snprintf(feedback_path, sizeof(feedback_path), "%s/apps/target-integration/history/migration_feedback.csv", root);
    awavma_runtime_config_default(&config); config.root_dir = root; config.bin_dir = bin_dir;
    config.phase_config_path = config_path; config.migration_safety_enabled = true;
    runtime = awavma_runtime_create();
    passed = ticks != 0 && runtime != NULL && awavma_runtime_init(runtime, &config) == 0 &&
             awavma_runtime_test_submit_approved_migration(runtime, child, ticks,
                 AWAVMA_RUNTIME_TEST_TARGET_PAGE_ADDRESS_UNAVAILABLE, &stats) == 0 &&
             stats.target_provider_calls == 0 && stats.executor_calls == 0 &&
             stats.rollback_calls == 0 && stats.terminal_feedback_calls == 1;
    file = fopen(feedback_path, "r");
    while (file != NULL && fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, stats.attempt_id) != NULL && strstr(line, "PAGE_ADDRESS_SET_UNAVAILABLE") != NULL)
            rows++;
    if (file != NULL) fclose(file);
    passed = passed && rows == 1;
    printf("RPC01_PRODUCTION_PAGE_ADDRESS_ABSENCE: %s\n", passed ? "PASS" : "FAIL");
    awavma_runtime_destroy(runtime); kill(child, SIGTERM); waitpid(child, NULL, 0);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
