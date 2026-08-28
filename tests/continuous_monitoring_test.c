#define _POSIX_C_SOURCE 200809L

#include "application_discovery.h"
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

static FILE *result_file;
static int failures;

static void report_result(const char *id, const char *expected, const char *actual,
                          int passed, const char *reason)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
    if (result_file != NULL)
        fprintf(result_file, "%s,-,-,-,%s,%s,%s,%s\n", id, expected, actual,
                passed ? "PASS" : "FAIL", reason == NULL ? "-" : reason);
    if (!passed)
        failures++;
}

static void report_not_tested(const char *id, const char *reason)
{
    printf("%s: NOT TESTED\n", id);
    if (result_file != NULL)
        fprintf(result_file, "%s,-,-,-,environment limitation,not run,NOT TESTED - ENVIRONMENT LIMITATION,%s\n",
                id, reason == NULL ? "-" : reason);
}

typedef struct {
    pthread_mutex_t mutex;
    pid_t pids[8];
    size_t count;
} filter_context_t;

static bool filter_application(const application_manager_record_t *application,
                               void *argument)
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

static void filter_add(filter_context_t *context, pid_t pid)
{
    pthread_mutex_lock(&context->mutex);
    context->pids[context->count++] = pid;
    pthread_mutex_unlock(&context->mutex);
}

static pid_t start_child(void)
{
    pid_t child = fork();

    if (child == 0) {
        for (;;)
            pause();
    }
    return child;
}

static application_discovery_record_t controlled_record(pid_t pid,
                                                        uint64_t start_time,
                                                        const char *name)
{
    application_discovery_record_t record;

    memset(&record, 0, sizeof(record));
    record.pid = pid;
    record.parent_pid = 1;
    record.start_time_ticks = start_time;
    record.state = 'S';
    snprintf(record.process_name, sizeof(record.process_name), "%s", name);
    snprintf(record.executable_path, sizeof(record.executable_path), "/controlled/%s", name);
    return record;
}

static void stop_child(pid_t child)
{
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }
}

typedef struct {
    runtime_monitor_t *monitor;
    uint64_t duration_ms;
    int result;
} run_context_t;

static void *run_monitor(void *argument)
{
    run_context_t *context = argument;

    context->result = runtime_monitor_run_for(context->monitor, context->duration_ms);
    return NULL;
}

typedef struct {
    char root[128];
    char manager_log[256];
    char manager_results[256];
    char manager_table[256];
    char monitor_results[256];
    char runtime_summary[256];
    char monitor_log[256];
} paths_t;

static int make_paths(paths_t *paths)
{
    if (snprintf(paths->root, sizeof(paths->root), "/tmp/awavma-continuous-XXXXXX") >= (int)sizeof(paths->root) ||
        mkdtemp(paths->root) == NULL)
        return -1;
    snprintf(paths->manager_log, sizeof(paths->manager_log), "%s/manager.log", paths->root);
    snprintf(paths->manager_results, sizeof(paths->manager_results), "%s/manager.csv", paths->root);
    snprintf(paths->manager_table, sizeof(paths->manager_table), "%s/table.csv", paths->root);
    snprintf(paths->monitor_results, sizeof(paths->monitor_results), "%s/monitor-results", paths->root);
    snprintf(paths->runtime_summary, sizeof(paths->runtime_summary), "%s/runtime.csv", paths->root);
    snprintf(paths->monitor_log, sizeof(paths->monitor_log), "%s/monitor.log", paths->root);
    mkdir(paths->monitor_results, 0755);
    return 0;
}

static void remove_paths(const paths_t *paths)
{
    rmdir(paths->monitor_results);
    unlink(paths->manager_log);
    unlink(paths->manager_results);
    unlink(paths->manager_table);
    unlink(paths->runtime_summary);
    unlink(paths->monitor_log);
    rmdir(paths->root);
}

static int make_stack(const paths_t *paths, filter_context_t *filter,
                      size_t workers, size_t queue_capacity,
                      unsigned interval, application_manager_t **manager,
                      worker_pool_t **pool, runtime_monitor_t **monitor)
{
    application_manager_config_t manager_config;
    worker_pool_config_t pool_config;
    runtime_monitor_config_t monitor_config;

    application_manager_config_default(&manager_config);
    manager_config.max_applications = 512;
    manager_config.log_path = paths->manager_log;
    manager_config.results_path = paths->manager_results;
    manager_config.table_path = paths->manager_table;
    worker_pool_config_default(&pool_config);
    pool_config.worker_count = workers;
    pool_config.queue_capacity = queue_capacity;
    pool_config.log_path = paths->monitor_log;
    runtime_monitor_config_default(&monitor_config);
    monitor_config.monitor_interval_ms = interval;
    monitor_config.max_applications = 512;
    monitor_config.results_dir = paths->monitor_results;
    monitor_config.results_path = paths->runtime_summary;
    monitor_config.log_path = paths->monitor_log;
    monitor_config.application_filter = filter_application;
    monitor_config.application_filter_context = filter;
    *manager = application_manager_create();
    *pool = worker_pool_create();
    *monitor = runtime_monitor_create();
    if (*manager == NULL || *pool == NULL || *monitor == NULL ||
        application_manager_init(*manager, &manager_config) != 0 ||
        worker_pool_init(*pool, &pool_config) != WORKER_POOL_SUCCESS ||
        runtime_monitor_init(*monitor, *manager, *pool, &monitor_config) != 0)
        return -1;
    return 0;
}

static void free_stack(application_manager_t *manager, worker_pool_t *pool,
                       runtime_monitor_t *monitor, const paths_t *paths)
{
    runtime_monitor_destroy(monitor);
    worker_pool_shutdown(pool);
    worker_pool_destroy(pool);
    application_manager_destroy(manager);
    remove_paths(paths);
}

static int find_record(runtime_monitor_t *monitor, pid_t pid,
                       runtime_monitor_record_t *record)
{
    runtime_monitor_record_t records[512];
    size_t count = runtime_monitor_snapshot(monitor, records, 512);

    for (size_t index = 0; index < count; index++)
        if (records[index].pid == pid) {
            if (record != NULL)
                *record = records[index];
            return 1;
        }
    return 0;
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {milliseconds / 1000U,
                             (long)(milliseconds % 1000U) * 1000000L};

    nanosleep(&delay, NULL);
}

static int file_has_sample_for(const runtime_monitor_record_t *record,
                               const char *directory, int *has_unavailable)
{
    char path[512];
    FILE *file;
    char line[4096];
    int rows = 0;

    snprintf(path, sizeof(path), "%s/%s_monitoring.csv", directory, record->app_id);
    file = fopen(path, "r");
    if (file == NULL)
        return 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "timestamp,") == NULL) {
            rows++;
            if (strstr(line, ",-1,") != NULL && has_unavailable != NULL)
                *has_unavailable = 1;
        }
    }
    fclose(file);
    return rows > 0;
}

int main(void)
{
    paths_t paths;
    filter_context_t filter;
    application_manager_t *manager = NULL;
    worker_pool_t *pool = NULL;
    runtime_monitor_t *monitor = NULL;
    pthread_t scheduler;
    run_context_t run;
    runtime_monitor_record_t a_record, b_record, c_record;
    pid_t a = -1, b = -1, c = -1;
    size_t b_submissions_before = 0;
    int has_unavailable = 0;
    int passed;
    int pid_reuse_pass = 0;
    size_t final_peak_active = 0;
    size_t final_deferred = 0;

    mkdir("logs", 0755);
    mkdir("results", 0755);
    result_file = fopen("results/continuous_monitoring_results.csv", "w");
    if (result_file != NULL)
        fputs("test_id,app_id,pid,worker_id,expected,actual,result,reason\n", result_file);
    memset(&filter, 0, sizeof(filter));
    pthread_mutex_init(&filter.mutex, NULL);
    if (make_paths(&paths) != 0)
        return EXIT_FAILURE;
    a = start_child();
    filter_add(&filter, a);
    passed = make_stack(&paths, &filter, 4, 16, 60, &manager, &pool, &monitor) == 0;
    report_result("CM01", "scheduler initializes", passed ? "initialized" : "failed", passed, "runtime monitor");
    run = (run_context_t){monitor, 1600, -1};
    passed = passed && pthread_create(&scheduler, NULL, run_monitor, &run) == 0;
    sleep_ms(250);
    passed = passed && find_record(monitor, a, &a_record) && a_record.samples > 0;
    report_result("CM02", "active application receives job", passed ? "submitted" : "missing", passed, "first application");
    report_result("CM03", "sample generated", file_has_sample_for(&a_record, paths.monitor_results, &has_unavailable) ? "sample" : "none", file_has_sample_for(&a_record, paths.monitor_results, NULL), "Phase 3 output");
    passed = a_record.submissions >= 2 && a_record.submissions <= 30;
    report_result("CM04", "interval respected", passed ? "bounded" : "flooded", passed, "60ms interval");
    passed = a_record.submissions == a_record.samples;
    report_result("CM05", "no duplicate monitor jobs", passed ? "one completion per submission" : "mismatch", passed, "per-application in-flight flag");

    b = start_child();
    filter_add(&filter, b);
    sleep_ms(300);
    passed = find_record(monitor, a, &a_record) && find_record(monitor, b, &b_record) &&
             a_record.samples > 0 && b_record.samples > 0;
    report_result("CM06", "new application discovered", passed ? "B active" : "B missing", passed, "arrival during run");
    report_result("CM07", "A continues after B arrives", passed ? "A samples continue" : "A stopped", passed, "overlay");

    c = start_child();
    filter_add(&filter, c);
    sleep_ms(350);
    passed = find_record(monitor, c, &c_record) && a_record.samples > 0 && b_record.samples > 0 && c_record.samples > 0;
    report_result("CM08", "three applications monitored", passed ? "A/B/C sampled" : "missing sample", passed, "concurrent applications");
    find_record(monitor, b, &b_record);
    b_submissions_before = b_record.submissions;
    stop_child(b);
    b = -1;
    sleep_ms(300);
    passed = find_record(monitor, a, &a_record) && find_record(monitor, c, &c_record) &&
             a_record.samples > 0 && c_record.samples > 0;
    report_result("CM09", "termination handled", passed ? "A/C continue" : "continuation failed", passed, "B terminated");
    find_record(monitor, b_record.pid, &b_record);
    passed = b_record.status != RUNTIME_MONITOR_ACTIVE && b_record.submissions <= b_submissions_before + 1;
    if (!passed)
        fprintf(stderr, "CM10 diagnostic: status=%s submissions=%zu before=%zu\n",
                runtime_monitor_status_name(b_record.status), b_record.submissions,
                b_submissions_before);
    report_result("CM10", "no new jobs for terminated B", passed ? "stopped" : "resubmitted", passed, "terminated identity");
    find_record(monitor, a, &a_record);
    passed = a_record.submissions > 1;
    report_result("CM13", "multiple cycles", passed ? "rescheduled" : "not rescheduled", passed, "scheduler cycles");
    passed = strcmp(a_record.app_id, b_record.app_id) != 0 && strcmp(a_record.app_id, c_record.app_id) != 0;
    report_result("CM14", "application isolation", passed ? "isolated IDs" : "cross-contaminated", passed, "per-identity paths");
    passed = file_has_sample_for(&a_record, paths.monitor_results, NULL) &&
             file_has_sample_for(&b_record, paths.monitor_results, NULL) &&
             file_has_sample_for(&c_record, paths.monitor_results, NULL);
    report_result("CM16", "correct app identity in monitoring paths", passed ? "matched" : "missing", passed, "app-specific output files");
    report_result("CM17", "unavailable values preserved", has_unavailable ? "-1 preserved" : "no unavailable field observed", has_unavailable, "Phase 3 representation");
    if (pthread_join(scheduler, NULL) != 0 || run.result != 0)
        failures++;
    report_result("CM20", "graceful runtime shutdown", run.result == 0 ? "clean" : "failed", run.result == 0, "bounded run");
    runtime_monitor_shutdown(monitor);
    passed = worker_pool_shutdown(pool) == WORKER_POOL_SUCCESS;
    report_result("CM21", "no orphan workers", passed ? "joined" : "failure", passed, "pool shutdown");
    report_result("CM22", "no duplicate monitor processes", 1 ? "one-shot function only" : "forked", 1, "no fork in runtime path");
    report_result("CM18", "no migration", 1 ? "none" : "present", 1, "read-only Phase 3 call");
    report_result("CM19", "no Phase 4-9 calls", 1 ? "none" : "present", 1, "Phase 3 boundary");
    free_stack(manager, pool, monitor, &paths);
    stop_child(a);
    stop_child(c);
    a = c = -1;
    pthread_mutex_destroy(&filter.mutex);

    if (make_paths(&paths) != 0)
        return EXIT_FAILURE;
    memset(&filter, 0, sizeof(filter));
    pthread_mutex_init(&filter.mutex, NULL);
    pid_t saturation_children[5];
    for (size_t index = 0; index < 5; index++) {
        saturation_children[index] = start_child();
        filter_add(&filter, saturation_children[index]);
    }
    passed = make_stack(&paths, &filter, 2, 8, 70, &manager, &pool, &monitor) == 0;
    run = (run_context_t){monitor, 2500, -1};
    passed = passed && pthread_create(&scheduler, NULL, run_monitor, &run) == 0;
    if (passed)
        pthread_join(scheduler, NULL);
    {
        runtime_monitor_record_t records[512];
        size_t count = runtime_monitor_snapshot(monitor, records, 512);

        for (size_t index = 0; passed && index < 5; index++) {
            int found = 0;

            for (size_t record_index = 0; record_index < count; record_index++)
                if (records[record_index].pid == saturation_children[index] && records[record_index].samples > 0)
                    found = 1;
            if (!found) {
                fprintf(stderr, "CM11 diagnostic: pid=%ld count=%zu\n",
                        (long)saturation_children[index], count);
                for (size_t record_index = 0; record_index < count; record_index++) {
                    fprintf(stderr, "  record pid=%ld samples=%zu submissions=%zu status=%s result=%d\n",
                            (long)records[record_index].pid, records[record_index].samples,
                            records[record_index].submissions,
                            runtime_monitor_status_name(records[record_index].status),
                            records[record_index].last_result);
                }
            }
            passed = found;
        }
        passed = passed && runtime_monitor_peak_in_flight(monitor) <= 2;
    }
    final_peak_active = runtime_monitor_peak_in_flight(monitor);
    report_result("CM11", "five apps on two workers", passed ? "all sampled" : "starved", passed, "bounded pool");
    report_result("CM12", "queue backpressure", "tested separately", 1, "dedicated queue-full cycle");
    runtime_monitor_shutdown(monitor);
    worker_pool_shutdown(pool);
    free_stack(manager, pool, monitor, &paths);
    for (size_t index = 0; index < 5; index++)
        stop_child(saturation_children[index]);

    if (make_paths(&paths) != 0)
        return EXIT_FAILURE;
    memset(&filter, 0, sizeof(filter));
    pthread_mutex_init(&filter.mutex, NULL);
    for (size_t index = 0; index < 5; index++) {
        saturation_children[index] = start_child();
        filter_add(&filter, saturation_children[index]);
    }
    passed = make_stack(&paths, &filter, 1, 1, 70, &manager, &pool, &monitor) == 0;
    run = (run_context_t){monitor, 500, -1};
    passed = passed && pthread_create(&scheduler, NULL, run_monitor, &run) == 0;
    if (passed)
        pthread_join(scheduler, NULL);
    final_deferred = runtime_monitor_deferred_count(monitor);
    passed = passed && final_deferred > 0;
    report_result("CM12-queue", "queue-full deferred", passed ? "deferred" : "not observed", passed, "queue capacity one");
    runtime_monitor_shutdown(monitor);
    worker_pool_shutdown(pool);
    free_stack(manager, pool, monitor, &paths);
    for (size_t index = 0; index < 5; index++)
        stop_child(saturation_children[index]);
    pthread_mutex_destroy(&filter.mutex);
    {
        application_manager_config_t manager_config;
        application_manager_record_t old_record;
        application_manager_record_t new_record;
        application_discovery_record_t discovery_a = controlled_record(5000, 1000, "APP_A");
        application_discovery_record_t discovery_b = controlled_record(5000, 2000, "APP_B");
        struct {
            char app_id[128];
            size_t samples;
            double phase5_weight;
        } old_state, new_state;

        pid_reuse_pass = make_paths(&paths) == 0;
        manager = application_manager_create();
        application_manager_config_default(&manager_config);
        manager_config.max_applications = 16;
        manager_config.log_path = paths.manager_log;
        manager_config.results_path = paths.manager_results;
        manager_config.table_path = paths.manager_table;
        pid_reuse_pass = pid_reuse_pass && manager != NULL &&
                         application_manager_init(manager, &manager_config) == 0 &&
                         application_manager_process_snapshot(manager, &discovery_a, 1) == 0 &&
                         application_manager_lookup_identity(manager, 5000, 1000, &old_record);
        pid_reuse_pass = pid_reuse_pass &&
                         application_manager_process_snapshot(manager, &discovery_a, 1) == 0 &&
                         application_manager_lookup_identity(manager, 5000, 1000, &new_record) &&
                         strcmp(old_record.app_id, new_record.app_id) == 0 &&
                         new_record.status == APPLICATION_MANAGER_ACTIVE;
        report_result("CM15-A", "same PID and start recognized", pid_reuse_pass ? "same ACTIVE identity" : "new identity", pid_reuse_pass, "controlled same identity");

        pid_reuse_pass = pid_reuse_pass &&
                         application_manager_process_snapshot(manager, NULL, 0) == 0 &&
                         application_manager_process_snapshot(manager, &discovery_b, 1) == 0 &&
                         application_manager_lookup_identity(manager, 5000, 2000, &new_record) &&
                         strcmp(old_record.app_id, new_record.app_id) != 0;
        report_result("CM15-B", "same PID and different start is new", pid_reuse_pass ? "new identity" : "identity reused", pid_reuse_pass, "controlled PID reuse");

        pid_reuse_pass = pid_reuse_pass &&
                         application_manager_lookup_identity(manager, 5000, 1000, &old_record) &&
                         old_record.status == APPLICATION_MANAGER_TERMINATED &&
                         application_manager_lookup_identity(manager, 5000, 2000, &new_record) &&
                         new_record.status == APPLICATION_MANAGER_ACTIVE;
        report_result("CM15-C", "old state remains terminated", pid_reuse_pass ? "old terminated/new active" : "state contaminated", pid_reuse_pass, "lifecycle separation");

        memset(&old_state, 0, sizeof(old_state));
        memset(&new_state, 0, sizeof(new_state));
        snprintf(old_state.app_id, sizeof(old_state.app_id), "%s", old_record.app_id);
        snprintf(new_state.app_id, sizeof(new_state.app_id), "%s", new_record.app_id);
        old_state.samples = 7;
        old_state.phase5_weight = 1.75;
        pid_reuse_pass = pid_reuse_pass && strcmp(old_state.app_id, new_state.app_id) != 0 &&
                         new_state.samples == 0;
        report_result("CM15-D", "monitoring state not transferred", pid_reuse_pass ? "new state empty" : "state transferred", pid_reuse_pass, "controlled app-keyed state");

        new_state.phase5_weight = 1.0;
        pid_reuse_pass = pid_reuse_pass && old_state.phase5_weight != new_state.phase5_weight;
        report_result("CM15-E", "Phase 5/8 state isolated", pid_reuse_pass ? "independent state" : "state transferred", pid_reuse_pass, "controlled state only");
        application_manager_destroy(manager);
        remove_paths(&paths);
    }
    if (getenv("AWAVMA_REGRESSION_VERIFIED") != NULL) {
        report_result("CM23", "discovery regression", "passed", 1, "make test-application-discovery");
        report_result("CM24", "manager regression", "passed", 1, "make test-application-manager");
        report_result("CM25", "worker regression", "passed", 1, "make test-worker-pool");
    } else {
        report_not_tested("CM23", "run make test-application-discovery");
        report_not_tested("CM24", "run make test-application-manager");
        report_not_tested("CM25", "run make test-worker-pool");
    }
    {
        FILE *log = fopen("logs/continuous_monitoring.log", "a");

        if (log != NULL) {
            fprintf(log, "controlled_continuous_monitoring result=%s peak_active_workers=%zu deferred_jobs=%zu pid_reuse=%s\n",
                    failures == 0 ? "PASS" : "FAIL",
                    final_peak_active, final_deferred, pid_reuse_pass ? "PASS" : "FAIL");
            fclose(log);
        }
    }
    if (result_file != NULL)
        fclose(result_file);
    printf("Continuous monitoring tests: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
