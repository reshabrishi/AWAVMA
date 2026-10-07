#define _POSIX_C_SOURCE 200809L

#include "application_manager.h"
#include "runtime_monitor.h"
#include "worker_pool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { pid_t first; size_t count; } admission_t;

static bool admit_range(const application_discovery_record_t *record, void *context)
{
    const admission_t *admission = context;
    return record->pid >= admission->first && record->pid < admission->first + (pid_t)admission->count;
}

static int write_stat(const char *root, pid_t pid, uint64_t start)
{
    char directory[512], path[512];
    FILE *file;

    if (snprintf(directory, sizeof(directory), "%s/%ld", root, (long)pid) >= (int)sizeof(directory) ||
        snprintf(path, sizeof(path), "%s/stat", directory) >= (int)sizeof(path) ||
        mkdir(directory, 0700) != 0 || (file = fopen(path, "w")) == NULL)
        return -1;
    fprintf(file, "%ld (fixture) S 1", (long)pid);
    for (unsigned field = 5; field <= 21; field++) fprintf(file, " 0");
    fprintf(file, " %llu\n", (unsigned long long)start);
    return fclose(file);
}

static void remove_fixture(const char *root, size_t count)
{
    char path[512];
    for (size_t index = 0; index < count; index++) {
        snprintf(path, sizeof(path), "%s/%ld/stat", root, (long)(100 + index)); unlink(path);
        snprintf(path, sizeof(path), "%s/%ld", root, (long)(100 + index)); rmdir(path);
    }
    snprintf(path, sizeof(path), "%s/999/stat", root); unlink(path);
    snprintf(path, sizeof(path), "%s/999", root); rmdir(path);
    rmdir(root);
}

static int run_case(const char *proc_root, size_t max, const admission_t *admission,
                    int expected_result, size_t expected_entries)
{
    char root[] = "/tmp/awavma-admission-run-XXXXXX";
    char results_dir[512], results_path[512], log_path[512], manager_log[512], manager_results[512], table[512];
    application_manager_config_t manager_config;
    runtime_monitor_config_t monitor_config;
    application_manager_t *manager = NULL;
    runtime_monitor_t *monitor = NULL;
    worker_pool_t *pool = NULL;
    worker_pool_config_t pool_config;
    runtime_monitor_record_t records[16];
    int result = -1;

    if (mkdtemp(root) == NULL) return 0;
    snprintf(results_dir, sizeof(results_dir), "%s/phase3", root);
    snprintf(results_path, sizeof(results_path), "%s/results.csv", root);
    snprintf(log_path, sizeof(log_path), "%s/monitor.log", root);
    snprintf(manager_log, sizeof(manager_log), "%s/manager.log", root);
    snprintf(manager_results, sizeof(manager_results), "%s/manager-results.csv", root);
    snprintf(table, sizeof(table), "%s/table.csv", root);
    manager = application_manager_create(); pool = worker_pool_create(); monitor = runtime_monitor_create();
    application_manager_config_default(&manager_config);
    manager_config.max_applications = max;
    manager_config.log_path = manager_log; manager_config.results_path = manager_results; manager_config.table_path = table;
    worker_pool_config_default(&pool_config); pool_config.worker_count = 1; pool_config.queue_capacity = 16; pool_config.log_path = NULL;
    runtime_monitor_config_default(&monitor_config);
    monitor_config.monitor_interval_ms = 1; monitor_config.discovery_interval_ms = 0;
    monitor_config.max_applications = max; monitor_config.results_dir = results_dir;
    monitor_config.results_path = results_path; monitor_config.log_path = log_path;
    monitor_config.discovery_config.proc_root = proc_root;
    monitor_config.discovery_admission = admission == NULL ? NULL : admit_range;
    monitor_config.discovery_admission_context = (void *)admission;
    if (manager != NULL && pool != NULL && monitor != NULL &&
        application_manager_init(manager, &manager_config) == 0 &&
        worker_pool_init(pool, &pool_config) == WORKER_POOL_SUCCESS &&
        runtime_monitor_init(monitor, manager, pool, &monitor_config) == 0) {
        result = runtime_monitor_run_for(monitor, 1);
        result = result == expected_result && runtime_monitor_snapshot(monitor, records, 16) == expected_entries;
    }
    runtime_monitor_destroy(monitor); worker_pool_destroy(pool); application_manager_destroy(manager);
    unlink(results_path); unlink(log_path); unlink(manager_log); unlink(manager_results); unlink(table); rmdir(results_dir); rmdir(root);
    return result;
}

int main(void)
{
    char proc_root[] = "/tmp/awavma-admission-proc-XXXXXX";
    admission_t one = {100, 1}, four = {100, 4}, five = {100, 5};
    int passed;

    if (mkdtemp(proc_root) == NULL) return EXIT_FAILURE;
    for (size_t index = 0; index < 10; index++) if (write_stat(proc_root, (pid_t)(100 + index), 1000 + index) != 0) return EXIT_FAILURE;
    { char path[512]; snprintf(path, sizeof(path), "%s/999", proc_root); mkdir(path, 0700); snprintf(path, sizeof(path), "%s/999/stat", proc_root); FILE *file = fopen(path, "w"); if (file != NULL) { fputs("malformed\n", file); fclose(file); } }
    passed = run_case(proc_root, 4, &one, 0, 1); printf("RA01_MANY_NONELIGIBLE_ONE_ELIGIBLE: %s\n", passed ? "PASS" : "FAIL");
    passed &= run_case(proc_root, 4, &four, 0, 4); printf("RA02_ELIGIBLE_EQUALS_CAPACITY: %s\n", passed ? "PASS" : "FAIL");
    passed &= run_case(proc_root, 4, &five, EIO, 0); printf("RA03_ELIGIBLE_EXCEEDS_CAPACITY: %s\n", passed ? "PASS" : "FAIL");
    passed &= run_case(proc_root, 4, NULL, EIO, 0); printf("RA04_NO_FILTER_PRESERVES_CAPACITY: %s\n", passed ? "PASS" : "FAIL");
    passed &= run_case(proc_root, 4, &one, 0, 1); printf("RA05_MALFORMED_RECORD_NOT_ADMITTED: %s\n", passed ? "PASS" : "FAIL");
    remove_fixture(proc_root, 10);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
