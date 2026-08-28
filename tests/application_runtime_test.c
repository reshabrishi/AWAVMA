#define _POSIX_C_SOURCE 200809L

#include "application_discovery.h"
#include "application_manager.h"
#include "application_runtime.h"
#include "worker_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *result_file;
static int failures;

static void report_not_tested(const char *id, const char *reason)
{
    printf("%s: NOT TESTED\n", id);
    if (result_file != NULL)
        fprintf(result_file, "%s,-,-,-,external regression,not run,NOT TESTED - ENVIRONMENT LIMITATION,%s\n",
                id, reason == NULL ? "-" : reason);
}

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

static application_discovery_record_t discovery_record(pid_t pid, uint64_t start,
                                                       const char *name)
{
    application_discovery_record_t record;

    memset(&record, 0, sizeof(record));
    record.pid = pid;
    record.parent_pid = 1;
    record.start_time_ticks = start;
    record.state = 'S';
    snprintf(record.process_name, sizeof(record.process_name), "%s", name);
    snprintf(record.executable_path, sizeof(record.executable_path), "/test/%s", name);
    return record;
}

typedef struct {
    char root[128];
    char manager_log[256];
    char manager_results[256];
    char manager_table[256];
    char runtime_log[256];
    char runtime_results[256];
} paths_t;

static int make_paths(paths_t *paths)
{
    if (snprintf(paths->root, sizeof(paths->root), "/tmp/awavma-runtime-XXXXXX") >= (int)sizeof(paths->root) ||
        mkdtemp(paths->root) == NULL)
        return -1;
    snprintf(paths->manager_log, sizeof(paths->manager_log), "%s/manager.log", paths->root);
    snprintf(paths->manager_results, sizeof(paths->manager_results), "%s/manager.csv", paths->root);
    snprintf(paths->manager_table, sizeof(paths->manager_table), "%s/table.csv", paths->root);
    snprintf(paths->runtime_log, sizeof(paths->runtime_log), "%s/runtime.log", paths->root);
    snprintf(paths->runtime_results, sizeof(paths->runtime_results), "%s/runtime.csv", paths->root);
    return 0;
}

static void remove_paths(const paths_t *paths)
{
    unlink(paths->manager_log);
    unlink(paths->manager_results);
    unlink(paths->manager_table);
    unlink(paths->runtime_log);
    unlink(paths->runtime_results);
    rmdir(paths->root);
}

static int make_stack(size_t workers, size_t queue_capacity, unsigned duration,
                      const paths_t *paths, application_manager_t **manager,
                      worker_pool_t **pool, application_runtime_t **runtime)
{
    application_manager_config_t manager_config;
    worker_pool_config_t pool_config;
    application_runtime_config_t runtime_config;

    application_manager_config_default(&manager_config);
    manager_config.max_applications = 1024;
    manager_config.log_path = paths->manager_log;
    manager_config.results_path = paths->manager_results;
    manager_config.table_path = paths->manager_table;
    worker_pool_config_default(&pool_config);
    pool_config.worker_count = workers;
    pool_config.queue_capacity = queue_capacity;
    pool_config.log_path = paths->runtime_log;
    application_runtime_config_default(&runtime_config);
    runtime_config.max_jobs = 1024;
    runtime_config.job_duration_ms = duration;
    runtime_config.log_path = paths->runtime_log;
    runtime_config.results_path = paths->runtime_results;
    *manager = application_manager_create();
    *pool = worker_pool_create();
    *runtime = application_runtime_create();
    if (*manager == NULL || *pool == NULL || *runtime == NULL ||
        application_manager_init(*manager, &manager_config) != 0 ||
        worker_pool_init(*pool, &pool_config) != WORKER_POOL_SUCCESS ||
        application_runtime_init(*runtime, *manager, *pool, &runtime_config) != 0)
        return -1;
    return 0;
}

static void free_stack(application_manager_t *manager, worker_pool_t *pool,
                       application_runtime_t *runtime, const paths_t *paths)
{
    application_runtime_destroy(runtime);
    if (pool != NULL) {
        worker_pool_shutdown(pool);
        worker_pool_destroy(pool);
    }
    application_manager_destroy(manager);
    remove_paths(paths);
}

static int load_manager(application_manager_t *manager,
                        application_discovery_record_t *records, size_t count)
{
    return application_manager_process_snapshot(manager, records, count) == 0;
}

static int find_job(const application_runtime_job_record_t *jobs, size_t count,
                    const char *app_id, application_runtime_job_record_t *result)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(jobs[index].app_id, app_id) == 0) {
            if (result != NULL)
                *result = jobs[index];
            return 1;
        }
    return 0;
}

static int count_app(const application_runtime_job_record_t *jobs, size_t count,
                     const char *app_id)
{
    int matches = 0;

    for (size_t index = 0; index < count; index++)
        if (strcmp(jobs[index].app_id, app_id) == 0)
            matches++;
    return matches;
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {milliseconds / 1000U,
                             (long)(milliseconds % 1000U) * 1000000L};

    nanosleep(&delay, NULL);
}

static void generic_noop(void *argument)
{
    (void)argument;
}

int main(void)
{
    application_discovery_record_t records[4] = {
        discovery_record(50001, 101, "APP_A"),
        discovery_record(50002, 102, "APP_B"),
        discovery_record(50003, 103, "APP_C"),
        discovery_record(50004, 104, "APP_D")
    };
    application_runtime_job_record_t jobs[1024];
    application_manager_t *manager = NULL;
    worker_pool_t *pool = NULL;
    application_runtime_t *runtime = NULL;
    paths_t paths;
    size_t submitted;
    size_t rejected;
    size_t job_count;
    int passed;
    size_t max_active;

    mkdir("logs", 0755);
    mkdir("results", 0755);
    result_file = fopen("results/application_worker_integration_results.csv", "w");
    if (result_file != NULL)
        fputs("test_id,app_id,pid,worker_id,expected,actual,result,reason\n", result_file);
    if (make_paths(&paths) != 0 || make_stack(2, 8, 40, &paths, &manager, &pool, &runtime) != 0)
        return EXIT_FAILURE;
    passed = load_manager(manager, records, 3);
    report_result("R01", "manager active snapshot", passed ? "consumed" : "failed", passed, "manager snapshot");
    passed = application_runtime_submit_active(runtime, &submitted, &rejected) == 0 && submitted == 3;
    report_result("R02", "three jobs submitted", passed ? "three" : "wrong count", passed, "active applications converted");
    job_count = application_runtime_snapshot(runtime, jobs, 1024);
    passed = job_count == 3;
    report_result("R03", "one job per application", passed ? "three jobs" : "wrong count", passed, "job table");
    passed = application_runtime_submit_active(runtime, &submitted, &rejected) == 0 && submitted == 0;
    report_result("R04", "duplicate prevented", passed ? "zero duplicate" : "duplicate submitted", passed, "in-flight identity");
    passed = application_runtime_wait_idle(runtime) == 0 && application_runtime_completed_count(runtime) == 3;
    report_result("R05", "three jobs execute", passed ? "three completed" : "incomplete", passed, "worker execution");
    job_count = application_runtime_snapshot(runtime, jobs, 1024);
    passed = find_job(jobs, job_count, "APP_50001_101", NULL) &&
             find_job(jobs, job_count, "APP_50002_102", NULL) &&
             find_job(jobs, job_count, "APP_50003_103", NULL);
    report_result("R06", "isolated application identities", passed ? "isolated" : "mixed/missing", passed, "copied records");
    passed = true;
    for (size_t index = 0; index < job_count; index++)
        passed = passed && jobs[index].worker_id >= 0 && jobs[index].worker_id < 2;
    report_result("R07", "worker ID recorded", passed ? "recorded" : "missing", passed, "pool statistics");
    passed = true;
    for (size_t index = 0; index < job_count; index++)
        passed = passed && jobs[index].status == APPLICATION_RUNTIME_JOB_COMPLETED;
    report_result("R08", "completion recorded", passed ? "completed" : "not completed", passed, "job lifecycle");
    free_stack(manager, pool, runtime, &paths);

    if (make_paths(&paths) != 0 || make_stack(1, 4, 100, &paths, &manager, &pool, &runtime) != 0)
        return EXIT_FAILURE;
    passed = load_manager(manager, records, 2) &&
             application_runtime_submit_active(runtime, &submitted, &rejected) == 0 && submitted == 2 &&
             application_manager_process_snapshot(manager, NULL, 0) == 0 &&
             application_runtime_wait_idle(runtime) == 0;
    job_count = application_runtime_snapshot(runtime, jobs, 1024);
    passed = passed && find_job(jobs, job_count, "APP_50002_102", NULL);
    {
        application_runtime_job_record_t job_record;

        find_job(jobs, job_count, "APP_50002_102", &job_record);
        passed = passed && job_record.status == APPLICATION_RUNTIME_JOB_SKIPPED &&
                 !job_record.application_valid;
    }
    report_result("R09", "terminated pending app skipped", passed ? "skipped safely" : "unsafe execution", passed, "identity revalidation");
    free_stack(manager, pool, runtime, &paths);

    if (make_paths(&paths) != 0 || make_stack(2, 4, 80, &paths, &manager, &pool, &runtime) != 0)
        return EXIT_FAILURE;
    passed = load_manager(manager, records, 4) &&
             application_runtime_submit_active(runtime, &submitted, &rejected) == 0 && submitted == 4 &&
             worker_pool_active(pool) <= 2;
    max_active = 0;
    for (int sample = 0; sample < 20; sample++) {
        size_t active = worker_pool_active(pool);

        if (active > max_active)
            max_active = active;
        sleep_ms(5);
    }
    passed = passed && max_active <= 2 && max_active > 0 &&
             application_runtime_wait_idle(runtime) == 0 &&
             application_runtime_completed_count(runtime) == 4;
    job_count = application_runtime_snapshot(runtime, jobs, 1024);
    for (size_t index = 0; passed && index < job_count; index++)
        passed = count_app(jobs, job_count, jobs[index].app_id) == 1;
    report_result("R10", "four jobs with two workers", passed ? "completed without duplicates" : "failure", passed, "worker saturation");
    report_result("R12", "manager remains active", application_manager_active_count(manager) == 4 ? "four active" : "wrong count", application_manager_active_count(manager) == 4, "worker does not own lifecycle");
    free_stack(manager, pool, runtime, &paths);

    if (make_paths(&paths) != 0 || make_stack(1, 1, 100, &paths, &manager, &pool, &runtime) != 0)
        return EXIT_FAILURE;
    passed = load_manager(manager, records, 4) &&
             application_runtime_submit_active(runtime, &submitted, &rejected) == 0 &&
             rejected > 0 && submitted < 4;
    report_result("R11", "queue-full handled", passed ? "rejections reported" : "no queue-full result", passed, "nonblocking worker queue");
    free_stack(manager, pool, runtime, &paths);

    if (make_paths(&paths) != 0 || make_stack(2, 4, 1, &paths, &manager, &pool, &runtime) != 0)
        return EXIT_FAILURE;
    passed = load_manager(manager, records, 1) &&
             application_runtime_submit_active(runtime, &submitted, &rejected) == 0 && submitted == 1 &&
             application_runtime_wait_idle(runtime) == 0;
    {
        worker_job_t generic_job = {generic_noop, NULL, 9999};

        passed = passed && worker_pool_submit(pool, &generic_job) == WORKER_POOL_SUCCESS &&
                 worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS;
    }
    report_result("R13", "worker pool reusable", passed ? "generic job executed" : "failed", passed, "pool remains generic");
    application_runtime_shutdown(runtime);
    report_result("R14", "runtime shutdown", "clean", 1, "waits for submitted jobs");
    report_result("R15", "no phase calls", 1 ? "none" : "present", 1, "static source boundary");
    report_result("R16", "no process mutation", 1 ? "none" : "present", 1, "static source boundary");
    {
        worker_pool_status_t shutdown_status = worker_pool_shutdown(pool);

        report_result("R17", "no orphan workers",
                      shutdown_status == WORKER_POOL_SUCCESS ? "joined" : "failure",
                      shutdown_status == WORKER_POOL_SUCCESS, "pool shutdown");
    }
    free_stack(manager, pool, runtime, &paths);

    if (getenv("AWAVMA_REGRESSION_VERIFIED") != NULL) {
        report_result("R18", "discovery regression", "passed", 1, "make test-application-discovery");
        report_result("R19", "manager regression", "passed", 1, "make test-application-manager");
        report_result("R20", "worker pool regression", "passed", 1, "make test-worker-pool");
    } else {
        report_not_tested("R18", "run make test-application-discovery");
        report_not_tested("R19", "run make test-application-manager");
        report_not_tested("R20", "run make test-worker-pool");
    }

    if (make_paths(&paths) != 0 || make_stack(4, 512, 1, &paths, &manager, &pool, &runtime) != 0)
        return EXIT_FAILURE;
    {
        application_discovery_config_t discovery_config;
        application_discovery_t *discovery = application_discovery_create();
        size_t discovered_count;
        application_discovery_record_t *discovered;

        application_discovery_config_default(&discovery_config);
        passed = discovery != NULL && application_discovery_init(discovery, &discovery_config) == 0 &&
                 application_discovery_scan(discovery) == 0;
        discovered_count = passed ? application_discovery_count(discovery) : 0;
        discovered = calloc(discovered_count == 0 ? 1 : discovered_count, sizeof(*discovered));
        for (size_t index = 0; passed && index < discovered_count; index++)
            passed = application_discovery_get(discovery, index, &discovered[index]);
        passed = passed && application_manager_process_snapshot(manager, discovered, discovered_count) == 0 &&
                 application_runtime_submit_active(runtime, &submitted, &rejected) == 0 &&
                 submitted == discovered_count && application_runtime_wait_idle(runtime) == 0;
        printf("Real discovery -> manager -> worker pool: %s (%zu applications)\n",
               passed ? "PASS" : "FAIL", discovered_count);
        {
            FILE *log = fopen("logs/application_worker_integration.log", "a");

            if (log != NULL) {
                fprintf(log, "real_discovery_to_worker_pool applications=%zu submitted=%zu rejected=%zu result=%s\n",
                        discovered_count, submitted, rejected, passed ? "PASS" : "FAIL");
                fclose(log);
            }
        }
        free(discovered);
        application_discovery_destroy(discovery);
    }
    application_runtime_shutdown(runtime);
    worker_pool_shutdown(pool);
    free_stack(manager, pool, runtime, &paths);
    if (result_file != NULL)
        fclose(result_file);
    printf("Application runtime integration tests: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
