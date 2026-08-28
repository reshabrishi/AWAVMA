#define _POSIX_C_SOURCE 200809L

#include "application_manager.h"
#include "runtime_monitor.h"
#include "worker_pool.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mutex;
    pid_t pids[2];
    size_t count;
} filter_context_t;

typedef struct {
    runtime_monitor_t *monitor;
    int result;
} run_context_t;

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool filter(const application_manager_record_t *application, void *argument)
{
    filter_context_t *context = argument;
    bool found = false;

    pthread_mutex_lock(&context->mutex);
    for (size_t index = 0; index < context->count; index++)
        if (context->pids[index] == application->pid)
            found = true;
    pthread_mutex_unlock(&context->mutex);
    return found;
}

static pid_t child(void)
{
    pid_t pid = fork();

    if (pid == 0)
        for (;;)
            pause();
    return pid;
}

static void stop_child(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}

static void *run(void *argument)
{
    run_context_t *context = argument;

    context->result = runtime_monitor_run_for(context->monitor, 3000);
    return NULL;
}

static bool record_exists(runtime_monitor_t *monitor, pid_t pid)
{
    runtime_monitor_record_t records[8];
    size_t count = runtime_monitor_snapshot(monitor, records, 8);

    for (size_t index = 0; index < count; index++)
        if (records[index].pid == pid && records[index].samples > 0)
            return true;
    return false;
}

int main(int argc, char **argv)
{
    char root[] = "/tmp/awavma-discovery-probe-XXXXXX";
    char phase3[512], manager_log[512], manager_results[512], manager_table[512];
    char monitor_results[512], monitor_log[512];
    char *end = NULL;
    unsigned long interval;
    application_manager_config_t manager_config;
    worker_pool_config_t pool_config;
    runtime_monitor_config_t monitor_config;
    application_manager_t *manager;
    worker_pool_t *pool;
    runtime_monitor_t *monitor;
    filter_context_t context;
    runtime_monitor_discovery_stats_t stats;
    run_context_t run_context;
    pthread_t thread;
    pid_t a = -1, b = -1;
    uint64_t started, latency = 0;
    int status = 1;

    if (argc != 2 || argv[1][0] == '-')
        return EXIT_FAILURE;
    errno = 0;
    interval = strtoul(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || interval == 0 || interval > 10000)
        return EXIT_FAILURE;
    if (mkdtemp(root) == NULL)
        return EXIT_FAILURE;
    snprintf(phase3, sizeof(phase3), "%s/phase3", root);
    snprintf(manager_log, sizeof(manager_log), "%s/manager.log", root);
    snprintf(manager_results, sizeof(manager_results), "%s/manager.csv", root);
    snprintf(manager_table, sizeof(manager_table), "%s/table.csv", root);
    snprintf(monitor_results, sizeof(monitor_results), "%s/monitor.csv", root);
    snprintf(monitor_log, sizeof(monitor_log), "%s/monitor.log", root);
    mkdir(phase3, 0755);
    memset(&context, 0, sizeof(context));
    pthread_mutex_init(&context.mutex, NULL);
    a = child();
    context.pids[context.count++] = a;
    application_manager_config_default(&manager_config);
    manager_config.max_applications = 512;
    manager_config.log_path = manager_log;
    manager_config.results_path = manager_results;
    manager_config.table_path = manager_table;
    worker_pool_config_default(&pool_config);
    pool_config.worker_count = 2;
    pool_config.queue_capacity = 8;
    runtime_monitor_config_default(&monitor_config);
    monitor_config.monitor_interval_ms = 30;
    monitor_config.discovery_interval_ms = interval;
    monitor_config.max_applications = 512;
    monitor_config.results_dir = phase3;
    monitor_config.results_path = monitor_results;
    monitor_config.log_path = monitor_log;
    monitor_config.application_filter = filter;
    monitor_config.application_filter_context = &context;
    manager = application_manager_create();
    pool = worker_pool_create();
    monitor = runtime_monitor_create();
    run_context = (run_context_t){monitor, -1};
    if (manager == NULL || pool == NULL || monitor == NULL ||
        application_manager_init(manager, &manager_config) != 0 ||
        worker_pool_init(pool, &pool_config) != WORKER_POOL_SUCCESS ||
        runtime_monitor_init(monitor, manager, pool, &monitor_config) != 0 ||
        pthread_create(&thread, NULL, run, &run_context) != 0)
        goto cleanup;
    for (unsigned attempts = 0; attempts < 200; attempts++) {
        if (runtime_monitor_discovery_stats(monitor, &stats) && stats.discovery_scan_count > 0)
            break;
        usleep(5000);
    }
    if (!runtime_monitor_discovery_stats(monitor, &stats) || stats.discovery_scan_count == 0)
        goto stop;
    started = monotonic_ms();
    b = child();
    pthread_mutex_lock(&context.mutex);
    context.pids[context.count++] = b;
    pthread_mutex_unlock(&context.mutex);
    for (unsigned attempts = 0; attempts < 800; attempts++) {
        if (record_exists(monitor, b)) {
            latency = monotonic_ms() - started;
            status = 0;
            break;
        }
        usleep(5000);
    }

stop:
    runtime_monitor_request_stop(monitor);
    pthread_join(thread, NULL);
    runtime_monitor_discovery_stats(monitor, &stats);
    printf("interval_ms,latency_ms,discovery_scans,schedule_passes,status\n");
    printf("%lu,%llu,%llu,%llu,%s\n", interval, (unsigned long long)latency,
           (unsigned long long)stats.discovery_scan_count,
           (unsigned long long)stats.schedule_pass_count, status == 0 ? "MEASURED" : "FAILED");

cleanup:
    runtime_monitor_destroy(monitor);
    worker_pool_shutdown(pool);
    worker_pool_destroy(pool);
    application_manager_destroy(manager);
    stop_child(a);
    stop_child(b);
    pthread_mutex_destroy(&context.mutex);
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
