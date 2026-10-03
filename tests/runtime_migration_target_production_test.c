#define _POSIX_C_SOURCE 200809L
#include "awavma_runtime.h"
#include "migration_target_provider.h"
#include "runtime_migration_metadata.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t start_ticks(pid_t pid)
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

static const char *live_policy_outcome(pid_t pid, uint64_t ticks, int *expected_source, int *expected_destination)
{
    MigrationTargetTopology topology;
    RuntimeMigrationMetadata metadata;
    bool physical_alternative = false;
    unsigned eligible_alternatives = 0;
    int source_node;

    if (expected_source == NULL || expected_destination == NULL ||
        !runtime_get_thread_migration_metadata(pid, pid, ticks, &metadata) ||
        !metadata.current_cpu_available || metadata.current_cpu < 0 ||
        !metadata.permitted_cpu_set_available || !migration_target_topology_read(&topology) ||
        !CPU_ISSET(metadata.current_cpu, &topology.online_cpus))
        return NULL;
    source_node = topology.cpu_node[metadata.current_cpu];
    if (source_node < 0)
        return NULL;
    for (unsigned node = 0; node < MIGRATION_TARGET_MAX_NODES; node++) {
        bool online = false;
        bool permitted = false;

        if (!topology.node_present[node] || (int)node == source_node)
            continue;
        for (int candidate = 0; candidate < CPU_SETSIZE; candidate++) {
            if (topology.cpu_node[candidate] != (int)node ||
                !CPU_ISSET(candidate, &topology.online_cpus))
                continue;
            online = true;
            if (CPU_ISSET(candidate, &metadata.permitted_cpu_set))
                permitted = true;
        }
        if (!online)
            continue;
        physical_alternative = true;
        if (permitted) {
            eligible_alternatives++;
            if (*expected_destination < 0)
                *expected_destination = (int)node;
        }
    }
    if (!physical_alternative)
        return "NO_ALTERNATE_TARGET";
    *expected_source = source_node;
    return eligible_alternatives > 0 ? "REJECTED" : "TARGET_UNAVAILABLE";
}

static int persistent_rows(const char *path, const awavma_runtime_test_target_stats_t *stats,
                           pid_t pid, uint64_t ticks, const char *outcome)
{
    char line[4096]; int count = 0; FILE *file = fopen(path, "r");
    if (file == NULL) return 0;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, stats->attempt_id) != NULL && strstr(line, outcome) != NULL &&
            strstr(line, ",target-integration,") != NULL) {
            char pid_text[32], tick_text[32];
            snprintf(pid_text, sizeof(pid_text), ",%ld,", (long)pid);
            snprintf(tick_text, sizeof(tick_text), ",%llu,", (unsigned long long)ticks);
            if (strstr(line, pid_text) != NULL && strstr(line, tick_text) != NULL) count++;
        }
    fclose(file); return count;
}

static int run_case(awavma_runtime_t *runtime, pid_t pid, uint64_t ticks,
                    awavma_runtime_test_target_case_t target_case, const char *outcome,
                    const char *feedback_path, int expected_source, int expected_destination)
{
    awavma_runtime_test_target_stats_t stats;
    return awavma_runtime_test_submit_approved_migration(runtime, pid, ticks, target_case, &stats) == 0 &&
            stats.target_provider_calls == 1 && stats.executor_calls == 0 && stats.rollback_calls == 0 &&
            stats.terminal_feedback_calls == 1 && stats.attempt_id[0] != '\0' &&
            (expected_destination < 0 || (stats.target_selected &&
                                          stats.target_source_numa_node == expected_source &&
                                          stats.target_destination_numa_node == expected_destination)) &&
           persistent_rows(feedback_path, &stats, pid, ticks, outcome) == 1;
}

int main(void)
{
    char root[] = "/tmp/awavma-target-production-XXXXXX", cwd[PATH_MAX], bin_dir[PATH_MAX + 32];
    char config_path[PATH_MAX + 32], feedback_path[PATH_MAX + 256];
    awavma_runtime_config_t config; awavma_runtime_t *runtime; pid_t child; uint64_t ticks;
    const char *live_outcome; int live_source = -1, live_destination = -1; bool initialized; bool test_passed; bool suite_passed = true;
    if (mkdtemp(root) == NULL || getcwd(cwd, sizeof(cwd)) == NULL) return EXIT_FAILURE;
    child = fork(); if (child == 0) for (;;) pause(); if (child < 0) return EXIT_FAILURE;
    usleep(10000); ticks = start_ticks(child);
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cwd);
    snprintf(config_path, sizeof(config_path), "%s/config/awavma.conf", cwd);
    snprintf(feedback_path, sizeof(feedback_path), "%s/apps/target-integration/history/migration_feedback.csv", root);
    awavma_runtime_config_default(&config); config.root_dir = root; config.bin_dir = bin_dir;
    config.phase_config_path = config_path; config.migration_safety_enabled = true;
    runtime = awavma_runtime_create();
    initialized = ticks != 0 && runtime != NULL && awavma_runtime_init(runtime, &config) == 0;

    if (initialized) {
        awavma_runtime_shutdown(runtime);
        initialized = awavma_runtime_init(runtime, &config) == 0;
    }
    printf("AR08_PLACEMENT_ALLOCATOR_SURVIVES_INIT: %s\n", initialized ? "PASS" : "FAIL");
    suite_passed = suite_passed && initialized;

    live_outcome = initialized ? live_policy_outcome(child, ticks, &live_source, &live_destination) : NULL;
    test_passed = live_outcome != NULL &&
                  run_case(runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_POLICY_LIVE,
                            live_outcome, feedback_path, live_source, live_destination);
    printf("RTPI00_LIVE_POLICY_TOPOLOGY_CORRECT: %s\n", test_passed ? "PASS" : "FAIL");
    suite_passed = suite_passed && test_passed;

    test_passed = initialized &&
                  run_case(runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_NO_ALTERNATE,
                            "NO_ALTERNATE_TARGET", feedback_path, -1, -1);
    printf("RTPI01_NO_ALTERNATE_PRODUCTION: %s\n", test_passed ? "PASS" : "FAIL");
    suite_passed = suite_passed && test_passed;

    test_passed = initialized &&
                  run_case(runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_UNAVAILABLE,
                            "TARGET_UNAVAILABLE", feedback_path, -1, -1);
    printf("RTPI02_TARGET_UNAVAILABLE_PRODUCTION: %s\n", test_passed ? "PASS" : "FAIL");
    suite_passed = suite_passed && test_passed;

    test_passed = initialized &&
                  run_case(runtime, child, ticks, AWAVMA_RUNTIME_TEST_TARGET_INVALID,
                            "TARGET_INVALID", feedback_path, -1, -1);
    printf("RTPI03_TARGET_INVALID_PRODUCTION: %s\n", test_passed ? "PASS" : "FAIL");
    suite_passed = suite_passed && test_passed;
    awavma_runtime_destroy(runtime); kill(child, SIGTERM); waitpid(child, NULL, 0);
    return suite_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
