#define _POSIX_C_SOURCE 200809L

#include "application_manager.h"
#include "runtime_monitor.h"
#include "worker_pool.h"

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
    pid_t pids[4];
    size_t count;
} filter_context_t;

typedef struct {
    runtime_monitor_t *monitor;
    uint64_t duration_ms;
    int result;
} run_context_t;

static int failures;

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void result(const char *id, int passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
    if (!passed)
        failures++;
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {milliseconds / 1000U, (long)(milliseconds % 1000U) * 1000000L};

    nanosleep(&delay, NULL);
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

static void add_filter(filter_context_t *context, pid_t pid)
{
    pthread_mutex_lock(&context->mutex);
    context->pids[context->count++] = pid;
    pthread_mutex_unlock(&context->mutex);
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

    context->result = runtime_monitor_run_for(context->monitor, context->duration_ms);
    return NULL;
}

static int lookup(runtime_monitor_t *monitor, pid_t pid, runtime_monitor_record_t *out)
{
    runtime_monitor_record_t records[32];
    size_t count = runtime_monitor_snapshot(monitor, records, 32);

    for (size_t index = 0; index < count; index++)
        if (records[index].pid == pid) {
            if (out != NULL)
                *out = records[index];
            return 1;
        }
    return 0;
}

int main(void)
{
    char root[] = "/tmp/awavma-discovery-cadence-XXXXXX";
    char phase3[512];
    char manager_log[512];
    char manager_results[512];
    char manager_table[512];
    char monitor_results[512];
    char monitor_log[512];
    application_manager_config_t manager_config;
    worker_pool_config_t pool_config;
    runtime_monitor_config_t monitor_config;
    application_manager_t *manager = NULL;
    worker_pool_t *pool = NULL;
    runtime_monitor_t *monitor = NULL;
    filter_context_t context;
    run_context_t run_context;
    runtime_monitor_discovery_stats_t before, after;
    runtime_monitor_record_t a_record, b_record;
    pthread_t thread;
    pid_t a = -1, b = -1;
    int passed;

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
    add_filter(&context, a);
    application_manager_config_default(&manager_config);
    manager_config.max_applications = 512;
    manager_config.log_path = manager_log;
    manager_config.results_path = manager_results;
    manager_config.table_path = manager_table;
    worker_pool_config_default(&pool_config);
    pool_config.worker_count = 2;
    pool_config.queue_capacity = 8;
    pool_config.log_path = NULL;
    runtime_monitor_config_default(&monitor_config);
    monitor_config.monitor_interval_ms = 30;
    monitor_config.discovery_interval_ms = 200;
    monitor_config.max_applications = 512;
    monitor_config.results_dir = phase3;
    monitor_config.results_path = monitor_results;
    monitor_config.log_path = monitor_log;
    monitor_config.application_filter = filter;
    monitor_config.application_filter_context = &context;
    manager = application_manager_create();
    pool = worker_pool_create();
    monitor = runtime_monitor_create();
    passed = manager != NULL && pool != NULL && monitor != NULL &&
             application_manager_init(manager, &manager_config) == 0 &&
             worker_pool_init(pool, &pool_config) == WORKER_POOL_SUCCESS &&
             runtime_monitor_init(monitor, manager, pool, &monitor_config) == 0;
    result("DC00", passed);
    run_context = (run_context_t){monitor, 900, -1};
    if (passed)
        passed = pthread_create(&thread, NULL, run, &run_context) == 0;
    sleep_ms(130);
    passed = passed && runtime_monitor_discovery_stats(monitor, &before) &&
             lookup(monitor, a, &a_record) && a_record.samples >= 2;
    result("DC03", passed);
    passed = passed && before.discovery_scan_count == 1 && before.schedule_pass_count > 1 &&
             before.discovery_skipped_not_due_count > 0;
    result("DC01", passed);
    result("DC02", passed);
    b = child();
    add_filter(&context, b);
    sleep_ms(300);
    passed = lookup(monitor, b, &b_record) && b_record.samples > 0 &&
             runtime_monitor_discovery_stats(monitor, &after) && after.discovery_scan_count >= 2;
    result("DC04", passed);
    passed = a_record.samples >= 2 && a_record.submissions == a_record.samples;
    result("DC07", passed);
    result("DC08", passed);
    stop_child(a);
    a = -1;
    sleep_ms(100);
    passed = lookup(monitor, a_record.pid, &a_record) &&
             a_record.status != RUNTIME_MONITOR_ACTIVE && lookup(monitor, b, &b_record) &&
             b_record.samples > 0;
    result("DC05", passed);
    {
        uint64_t stop_started = monotonic_ms();

        runtime_monitor_request_stop(monitor);
        passed = pthread_join(thread, NULL) == 0 && run_context.result == 0 &&
                 monotonic_ms() - stop_started < 250;
    }
    result("DC09", passed);
    runtime_monitor_destroy(monitor);
    worker_pool_shutdown(pool);
    worker_pool_destroy(pool);
    application_manager_destroy(manager);
    stop_child(b);
    pthread_mutex_destroy(&context.mutex);

    {
        application_manager_t *reuse_manager = application_manager_create();
        application_manager_config_t config;
        application_discovery_record_t old = {0};
        application_discovery_record_t replacement = {0};
        application_manager_record_t old_record, replacement_record;

        old.pid = replacement.pid = 5000;
        old.start_time_ticks = 100;
        replacement.start_time_ticks = 200;
        old.state = replacement.state = 'S';
        snprintf(old.process_name, sizeof(old.process_name), "old");
        snprintf(replacement.process_name, sizeof(replacement.process_name), "replacement");
        application_manager_config_default(&config);
        config.max_applications = 8;
        config.log_path = NULL;
        config.results_path = NULL;
        config.table_path = NULL;
        passed = reuse_manager != NULL && application_manager_init(reuse_manager, &config) == 0 &&
                 application_manager_process_snapshot(reuse_manager, &old, 1) == 0 &&
                 application_manager_process_snapshot(reuse_manager, &replacement, 1) == 0 &&
                 application_manager_lookup_identity(reuse_manager, 5000, 100, &old_record) &&
                 application_manager_lookup_identity(reuse_manager, 5000, 200, &replacement_record) &&
                 old_record.status == APPLICATION_MANAGER_TERMINATED &&
                 replacement_record.status == APPLICATION_MANAGER_ACTIVE &&
                 strcmp(old_record.app_id, replacement_record.app_id) != 0;
        result("DC06", passed);
        application_manager_destroy(reuse_manager);
    }
    printf("Discovery cadence tests: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
