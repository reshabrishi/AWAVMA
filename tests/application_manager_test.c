#define _POSIX_C_SOURCE 200809L

#include "application_discovery.h"
#include "application_manager.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *result_file;
static int failures;

typedef struct {
    char log_path[512];
    char results_path[512];
    char table_path[512];
} test_paths_t;

static application_discovery_record_t process_record(pid_t pid, uint64_t start,
                                                     const char *name)
{
    application_discovery_record_t record;

    memset(&record, 0, sizeof(record));
    record.pid = pid;
    record.parent_pid = 1;
    record.start_time_ticks = start;
    record.state = 'S';
    snprintf(record.process_name, sizeof(record.process_name), "%s", name);
    snprintf(record.executable_path, sizeof(record.executable_path), "/bin/%s", name);
    return record;
}

static void report_result(const char *id, const char *expected, const char *actual,
                          int passed, const char *reason)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
    if (result_file != NULL)
        fprintf(result_file, "%s,%s,%s,%s,%s\n", id, expected, actual,
                passed ? "PASS" : "FAIL", reason == NULL ? "-" : reason);
    if (!passed)
        failures++;
}

static application_manager_t *new_manager(const char *root, unsigned id,
                                           uint64_t timeout, test_paths_t *paths)
{
    application_manager_config_t config;
    application_manager_t *manager;

    snprintf(paths->log_path, sizeof(paths->log_path), "%s/manager-%u.log", root, id);
    snprintf(paths->results_path, sizeof(paths->results_path), "%s/manager-%u-results.csv", root, id);
    snprintf(paths->table_path, sizeof(paths->table_path), "%s/manager-%u-table.csv", root, id);
    application_manager_config_default(&config);
    config.max_applications = 2048;
    config.application_idle_timeout_ms = timeout;
    config.log_path = paths->log_path;
    config.results_path = paths->results_path;
    config.table_path = paths->table_path;
    manager = application_manager_create();
    if (manager == NULL || application_manager_init(manager, &config) != 0) {
        application_manager_destroy(manager);
        return NULL;
    }
    return manager;
}

static void free_manager(application_manager_t *manager, const test_paths_t *paths)
{
    application_manager_destroy(manager);
    unlink(paths->log_path);
    unlink(paths->results_path);
    unlink(paths->table_path);
}

static int process_one(application_manager_t *manager,
                       application_discovery_record_t record)
{
    return application_manager_process_snapshot(manager, &record, 1) == 0;
}

static int lookup_one(application_manager_t *manager, pid_t pid, uint64_t start,
                      application_manager_record_t *record)
{
    return application_manager_lookup_identity(manager, pid, start, record);
}

typedef struct {
    application_manager_t *manager;
    char app_id[128];
    _Atomic int failed;
} reader_context_t;

static void *reader_thread(void *argument)
{
    reader_context_t *context = argument;

    for (int iteration = 0; iteration < 200; iteration++) {
        application_manager_record_t record;
        application_manager_record_t snapshot[2];

        if (!application_manager_lookup_app_id(context->manager, context->app_id, &record) ||
            application_manager_snapshot(context->manager, snapshot, 2) == 0)
            atomic_store(&context->failed, 1);
    }
    return NULL;
}

typedef struct {
    application_manager_t *manager;
    application_discovery_record_t records[2];
    _Atomic int *failed;
} updater_context_t;

static void *updater_thread(void *argument)
{
    updater_context_t *context = argument;

    for (int iteration = 0; iteration < 100; iteration++)
        if (application_manager_process_snapshot(context->manager, context->records, 2) != 0)
            atomic_store(context->failed, 1);
    return NULL;
}

int main(void)
{
    char temporary_root[] = "/tmp/awavma-manager-XXXXXX";
    application_manager_t *manager;
    application_manager_record_t first;
    application_manager_record_t second;
    application_manager_record_t snapshot[4];
    application_discovery_record_t a = process_record(100, 10, "app-a");
    application_discovery_record_t b = process_record(101, 20, "app-b");
    application_discovery_record_t c = process_record(102, 30, "app-c");
    test_paths_t paths;
    size_t count;
    int passed;

    if (mkdtemp(temporary_root) == NULL)
        return EXIT_FAILURE;
    result_file = fopen("results/application_manager_results.csv", "w");
    if (result_file != NULL)
        fputs("test_id,expected,actual,status,reason\n", result_file);

    manager = new_manager(temporary_root, 1, 0, &paths);
    report_result("M01", "initialized", manager == NULL ? "failed" : "initialized",
                  manager != NULL, "manager initialization");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 2, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &first) &&
             first.status == APPLICATION_MANAGER_ACTIVE;
    report_result("M02", "new process active", passed ? "active" : "missing", passed, "registration");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 3, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &first) &&
             process_one(manager, a) && lookup_one(manager, a.pid, a.start_time_ticks, &second) &&
             strcmp(first.app_id, second.app_id) == 0 && application_manager_active_count(manager) == 1;
    report_result("M03", "same identity recognized", passed ? "recognized" : "duplicate", passed, "second snapshot");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 4, 0, &paths);
    {
        application_discovery_record_t duplicate_records[] = {a, a, a};
        passed = manager != NULL && application_manager_process_snapshot(manager, duplicate_records, 3) == 0 &&
                 application_manager_active_count(manager) == 1;
    }
    report_result("M04", "one registration", passed ? "one registration" : "duplicate", passed, "duplicate input");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 5, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &first);
    sleep(1);
    passed = passed && process_one(manager, a) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &second) && second.last_seen > first.last_seen;
    report_result("M05", "last_seen advances", passed ? "updated" : "unchanged", passed, "observation time");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 6, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             application_manager_process_snapshot(manager, NULL, 0) == 0 &&
             lookup_one(manager, a.pid, a.start_time_ticks, &first) &&
             first.status == APPLICATION_MANAGER_TERMINATED;
    report_result("M06", "missing becomes terminated", passed ? "terminated" : "active", passed, "absent snapshot");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 7, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &first) &&
             application_manager_process_snapshot(manager, NULL, 0) == 0 &&
             process_one(manager, process_record(a.pid, 11, "app-a-new")) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &second) &&
             second.status == APPLICATION_MANAGER_TERMINATED &&
             lookup_one(manager, a.pid, 11, &second) && second.status == APPLICATION_MANAGER_ACTIVE &&
             strcmp(first.app_id, second.app_id) != 0;
    report_result("M07", "new start time creates identity", passed ? "PID reuse handled" : "identity reused", passed, "PID reuse");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 8, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             application_manager_process_snapshot(manager, NULL, 0) == 0 &&
             application_manager_active_count(manager) == 0 &&
             application_manager_snapshot(manager, snapshot, 4) == 0;
    report_result("M08", "terminated not active", passed ? "not active" : "active", passed, "active filtering");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 9, 20, &paths);
    {
        struct timespec delay = {0, 50000000L};

        passed = manager != NULL && process_one(manager, a) &&
                 application_manager_process_snapshot(manager, NULL, 0) == 0 &&
                 nanosleep(&delay, NULL) == 0 &&
                 application_manager_process_snapshot(manager, NULL, 0) == 0 &&
                 lookup_one(manager, a.pid, a.start_time_ticks, &first) &&
                 first.status == APPLICATION_MANAGER_EXPIRED;
    }
    report_result("M09", "stale becomes expired", passed ? "expired" : "not expired", passed, "idle timeout");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 10, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &first) &&
             application_manager_lookup_app_id(manager, first.app_id, &second) &&
             strcmp(second.app_id, first.app_id) == 0;
    report_result("M10", "lookup by app_id", passed ? "found" : "missing", passed, "application ID lookup");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 11, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &first) && first.pid == a.pid &&
             first.start_time_ticks == a.start_time_ticks;
    report_result("M11", "lookup by PID and start", passed ? "found" : "missing", passed, "identity lookup");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 12, 0, &paths);
    {
        application_discovery_record_t records[] = {a, b};
        passed = manager != NULL && application_manager_process_snapshot(manager, records, 2) == 0 &&
                 application_manager_active_count(manager) == 2;
    }
    report_result("M12", "two active applications", passed ? "2" : "wrong count", passed, "active count");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 13, 0, &paths);
    {
        application_discovery_record_t records[] = {a, b, c};

        passed = manager != NULL && application_manager_process_snapshot(manager, records, 3) == 0 &&
                 application_manager_snapshot(manager, snapshot, 4) == 3;
        count = passed ? 3 : 0;
        for (size_t index = 0; passed && index < count; index++)
            passed = snapshot[index].status == APPLICATION_MANAGER_ACTIVE;
    }
    report_result("M13", "active snapshot only", passed ? "3 active" : "unexpected records", passed, "snapshot copy");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 14, 0, &paths);
    passed = manager != NULL && process_one(manager, a) &&
             lookup_one(manager, a.pid, a.start_time_ticks, &first);
    if (passed) {
        reader_context_t context;
        pthread_t threads[2];
        int first_created = 0;
        int second_created = 0;

        memset(&context, 0, sizeof(context));
        context.manager = manager;
        snprintf(context.app_id, sizeof(context.app_id), "%s", first.app_id);
        atomic_init(&context.failed, 0);
        if (pthread_create(&threads[0], NULL, reader_thread, &context) == 0)
            first_created = 1;
        else
            passed = 0;
        if (first_created && pthread_create(&threads[1], NULL, reader_thread, &context) == 0)
            second_created = 1;
        else
            passed = 0;
        if (first_created)
            pthread_join(threads[0], NULL);
        if (second_created)
            pthread_join(threads[1], NULL);
        if (passed)
            passed = atomic_load(&context.failed) == 0;
    }
    report_result("M14", "safe concurrent lookup/snapshot", passed ? "safe" : "failure", passed, "mutex protected reads");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 15, 0, &paths);
    {
        updater_context_t context;
        pthread_t threads[2];
        _Atomic int update_failed = 0;
        application_discovery_record_t records[] = {a, b};
        int first_created = 0;
        int second_created = 0;

        context.manager = manager;
        context.records[0] = records[0];
        context.records[1] = records[1];
        context.failed = &update_failed;
        passed = manager != NULL;
        if (passed && pthread_create(&threads[0], NULL, updater_thread, &context) == 0)
            first_created = 1;
        else
            passed = 0;
        if (first_created && pthread_create(&threads[1], NULL, updater_thread, &context) == 0)
            second_created = 1;
        else
            passed = 0;
        if (first_created)
            pthread_join(threads[0], NULL);
        if (second_created)
            pthread_join(threads[1], NULL);
        if (passed)
            passed = atomic_load(&update_failed) == 0 && application_manager_active_count(manager) == 2;
    }
    report_result("M15", "safe concurrent registration/update", passed ? "safe" : "failure", passed, "mutex protected writes");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 16, 0, &paths);
    if (manager != NULL)
        application_manager_shutdown(manager);
    passed = manager != NULL && application_manager_active_count(manager) == 0;
    report_result("M16", "clean shutdown", passed ? "shutdown" : "failure", passed, "no worker lifecycle");
    free_manager(manager, &paths);

    manager = new_manager(temporary_root, 17, 0, &paths);
    passed = manager != NULL && process_one(manager, a) && application_manager_active_count(manager) == 1;
    report_result("M17", "no phase operation", passed ? "lifecycle only" : "failure", passed, "manager API only");
    free_manager(manager, &paths);

    {
        application_discovery_config_t discovery_config;
        application_discovery_t *discovery = application_discovery_create();
        size_t discovered_count = 0;

        manager = new_manager(temporary_root, 18, 0, &paths);
        application_discovery_config_default(&discovery_config);
        passed = discovery != NULL && manager != NULL &&
                 application_discovery_init(discovery, &discovery_config) == 0 &&
                 application_discovery_scan(discovery) == 0;
        if (passed) {
            application_discovery_record_t *records;

            discovered_count = application_discovery_count(discovery);
            records = calloc(discovered_count == 0 ? 1 : discovered_count, sizeof(*records));
            passed = records != NULL;
            for (size_t index = 0; passed && index < discovered_count; index++)
                passed = application_discovery_get(discovery, index, &records[index]);
            if (passed)
                passed = application_manager_process_snapshot(manager, records, discovered_count) == 0 &&
                         application_manager_active_count(manager) == discovered_count;
            free(records);
        }
        report_result("M18", "real discovery snapshot consumed", passed ? "consumed" : "failure", passed, "discovery integration");
        application_discovery_destroy(discovery);
    }
    free_manager(manager, &paths);

    if (result_file != NULL)
        fclose(result_file);
    rmdir(temporary_root);
    printf("Application manager tests: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
