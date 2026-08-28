#define _POSIX_C_SOURCE 200809L

#include "application_runtime.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_MAX_JOBS 1024U
#define DEFAULT_JOB_DURATION_MS 5U
#define DEFAULT_LOG_PATH "logs/application_worker_integration.log"
#define DEFAULT_RESULTS_PATH "results/application_worker_integration_results.csv"
#define RESULTS_HEADER "test_id,app_id,pid,worker_id,expected,actual,result,reason"

typedef struct {
    application_runtime_job_record_t record;
    application_manager_record_t application;
} runtime_job_entry_t;

struct application_runtime {
    application_manager_t *manager;
    worker_pool_t *pool;
    application_runtime_config_t config;
    runtime_job_entry_t *jobs;
    size_t count;
    size_t capacity;
    uint64_t next_job_id;
    pthread_mutex_t mutex;
    FILE *log_file;
    FILE *results_file;
    bool initialized;
    bool shutting_down;
};

typedef struct {
    application_runtime_t *runtime;
    size_t entry_index;
} runtime_job_context_t;

static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {milliseconds / 1000U,
                             (long)(milliseconds % 1000U) * 1000000L};

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static const char *status_name(application_runtime_job_status_t status)
{
    switch (status) {
    case APPLICATION_RUNTIME_JOB_PENDING: return "PENDING";
    case APPLICATION_RUNTIME_JOB_COMPLETED: return "COMPLETED";
    case APPLICATION_RUNTIME_JOB_SKIPPED: return "SKIPPED";
    case APPLICATION_RUNTIME_JOB_REJECTED: return "REJECTED";
    default: return "UNKNOWN";
    }
}

const char *application_runtime_job_status_name(application_runtime_job_status_t status)
{
    return status_name(status);
}

static void log_locked(application_runtime_t *runtime, const char *event,
                       const runtime_job_entry_t *entry, const char *reason)
{
    if (runtime->log_file == NULL)
        return;
    fprintf(runtime->log_file, "%lld,event=%s,job_id=%llu,app_id=%s,pid=%ld,worker_id=%d,status=%s,reason=%s\n",
            (long long)time(NULL), event,
            (unsigned long long)entry->record.job_id, entry->record.app_id,
            (long)entry->record.pid, entry->record.worker_id,
            status_name(entry->record.status), reason == NULL ? "-" : reason);
    fflush(runtime->log_file);
}

static void write_result_locked(application_runtime_t *runtime,
                                const runtime_job_entry_t *entry,
                                const char *expected, const char *reason)
{
    if (runtime->results_file == NULL)
        return;
    fprintf(runtime->results_file, "JOB,%s,%ld,%d,%s,%s,%s,%s\n",
            entry->record.app_id, (long)entry->record.pid,
            entry->record.worker_id, expected,
            entry->record.app_id, status_name(entry->record.status),
            reason == NULL ? "-" : reason);
    fflush(runtime->results_file);
}

static ssize_t find_job_locked(const application_runtime_t *runtime,
                               const char *app_id, pid_t pid,
                               uint64_t start_time_ticks)
{
    for (size_t index = 0; index < runtime->count; index++) {
        const application_runtime_job_record_t *record = &runtime->jobs[index].record;

        if (record->pid == pid && record->start_time_ticks == start_time_ticks &&
            strcmp(record->app_id, app_id) == 0)
            return (ssize_t)index;
    }
    return -1;
}

static bool job_in_flight(const runtime_job_entry_t *entry)
{
    return entry->record.status == APPLICATION_RUNTIME_JOB_PENDING;
}

static int append_job_locked(application_runtime_t *runtime,
                             const application_manager_record_t *application,
                             size_t *index)
{
    runtime_job_entry_t *expanded;
    size_t capacity;
    runtime_job_entry_t *entry;

    if (runtime->count >= runtime->config.max_jobs)
        return ENOSPC;
    if (runtime->count == runtime->capacity) {
        capacity = runtime->capacity == 0 ? 16U : runtime->capacity * 2U;
        if (capacity > runtime->config.max_jobs)
            capacity = runtime->config.max_jobs;
        expanded = realloc(runtime->jobs, capacity * sizeof(*expanded));
        if (expanded == NULL)
            return ENOMEM;
        runtime->jobs = expanded;
        runtime->capacity = capacity;
    }
    entry = &runtime->jobs[runtime->count];
    memset(entry, 0, sizeof(*entry));
    entry->record.job_id = runtime->next_job_id++;
    snprintf(entry->record.app_id, sizeof(entry->record.app_id), "%s", application->app_id);
    entry->record.pid = application->pid;
    entry->record.start_time_ticks = application->start_time_ticks;
    entry->record.worker_id = -1;
    entry->record.status = APPLICATION_RUNTIME_JOB_PENDING;
    entry->record.application_valid = true;
    entry->application = *application;
    *index = runtime->count++;
    return 0;
}

static int identify_worker(const worker_pool_t *pool, uint64_t job_id)
{
    worker_stats_t stats[WORKER_POOL_MAX_WORKERS];
    size_t count = worker_pool_snapshot(pool, stats, WORKER_POOL_MAX_WORKERS);

    for (size_t index = 0; index < count; index++)
        if (stats[index].busy && stats[index].current_job_id == job_id)
            return (int)stats[index].worker_id;
    return -1;
}

static void runtime_job(void *argument)
{
    runtime_job_context_t *context = argument;
    application_runtime_t *runtime = context->runtime;
    runtime_job_entry_t *entry;
    application_manager_record_t current;
    bool valid;
    int worker_id;

    pthread_mutex_lock(&runtime->mutex);
    entry = &runtime->jobs[context->entry_index];
    worker_id = identify_worker(runtime->pool, entry->record.job_id);
    entry->record.worker_id = worker_id;
    entry->record.status = APPLICATION_RUNTIME_JOB_PENDING;
    pthread_mutex_unlock(&runtime->mutex);

    valid = application_manager_lookup_identity(runtime->manager,
                                                entry->record.pid,
                                                entry->record.start_time_ticks,
                                                &current) &&
            current.status == APPLICATION_MANAGER_ACTIVE &&
            strcmp(current.app_id, entry->record.app_id) == 0;
    if (valid)
        sleep_ms(runtime->config.job_duration_ms);

    pthread_mutex_lock(&runtime->mutex);
    entry = &runtime->jobs[context->entry_index];
    entry->record.application_valid = valid;
    entry->record.status = valid ? APPLICATION_RUNTIME_JOB_COMPLETED : APPLICATION_RUNTIME_JOB_SKIPPED;
    log_locked(runtime, valid ? "JOB_COMPLETED" : "JOB_SKIPPED", entry,
               valid ? "application identity valid" : "application no longer active");
    write_result_locked(runtime, entry, valid ? "completed" : "skipped", valid ? "completed" : "identity invalid");
    pthread_mutex_unlock(&runtime->mutex);
    free(context);
}

void application_runtime_config_default(application_runtime_config_t *config)
{
    if (config == NULL)
        return;
    config->max_jobs = DEFAULT_MAX_JOBS;
    config->job_duration_ms = DEFAULT_JOB_DURATION_MS;
    config->log_path = DEFAULT_LOG_PATH;
    config->results_path = DEFAULT_RESULTS_PATH;
}

application_runtime_t *application_runtime_create(void)
{
    return calloc(1, sizeof(application_runtime_t));
}

int application_runtime_init(application_runtime_t *runtime,
                             application_manager_t *manager,
                             worker_pool_t *pool,
                             const application_runtime_config_t *config)
{
    application_runtime_config_t defaults;

    if (runtime == NULL || manager == NULL || pool == NULL)
        return EINVAL;
    if (config == NULL) {
        application_runtime_config_default(&defaults);
        config = &defaults;
    }
    if (config->max_jobs == 0)
        return EINVAL;
    memset(runtime, 0, sizeof(*runtime));
    runtime->manager = manager;
    runtime->pool = pool;
    runtime->config = *config;
    runtime->next_job_id = 1;
    if (pthread_mutex_init(&runtime->mutex, NULL) != 0)
        return EIO;
    if (config->log_path != NULL) {
        runtime->log_file = fopen(config->log_path, "a");
        if (runtime->log_file == NULL)
            goto fail;
    }
    if (config->results_path != NULL) {
        runtime->results_file = fopen(config->results_path, "w");
        if (runtime->results_file == NULL)
            goto fail;
        fprintf(runtime->results_file, "%s\n", RESULTS_HEADER);
        fflush(runtime->results_file);
    }
    runtime->initialized = true;
    return 0;

fail:
    if (runtime->results_file != NULL)
        fclose(runtime->results_file);
    if (runtime->log_file != NULL)
        fclose(runtime->log_file);
    pthread_mutex_destroy(&runtime->mutex);
    return EIO;
}

int application_runtime_submit_snapshot(application_runtime_t *runtime,
                                        const application_manager_record_t *records,
                                        size_t count, size_t *submitted,
                                        size_t *rejected)
{
    size_t accepted = 0;
    size_t refused = 0;

    if (runtime == NULL || !runtime->initialized || (count > 0 && records == NULL))
        return EINVAL;
    pthread_mutex_lock(&runtime->mutex);
    if (runtime->shutting_down) {
        pthread_mutex_unlock(&runtime->mutex);
        return ECANCELED;
    }
    for (size_t index = 0; index < count; index++) {
        runtime_job_context_t *context;
        worker_job_t job;
        size_t job_index;
        ssize_t existing;
        int append_status;
        worker_pool_status_t submit_status;

        if (records[index].status != APPLICATION_MANAGER_ACTIVE)
            continue;
        existing = find_job_locked(runtime, records[index].app_id, records[index].pid,
                                   records[index].start_time_ticks);
        if (existing >= 0 && job_in_flight(&runtime->jobs[existing]))
            continue;
        append_status = append_job_locked(runtime, &records[index], &job_index);
        if (append_status != 0) {
            refused++;
            continue;
        }
        context = malloc(sizeof(*context));
        if (context == NULL) {
            runtime->jobs[job_index].record.status = APPLICATION_RUNTIME_JOB_REJECTED;
            refused++;
            continue;
        }
        context->runtime = runtime;
        context->entry_index = job_index;
        job = (worker_job_t){runtime_job, context, runtime->jobs[job_index].record.job_id};
        submit_status = worker_pool_submit(runtime->pool, &job);
        if (submit_status != WORKER_POOL_SUCCESS) {
            runtime->jobs[job_index].record.status = APPLICATION_RUNTIME_JOB_REJECTED;
            runtime->jobs[job_index].record.application_valid = false;
            log_locked(runtime, "JOB_REJECTED", &runtime->jobs[job_index],
                       worker_pool_status_name(submit_status));
            write_result_locked(runtime, &runtime->jobs[job_index], "submitted", "worker pool rejected job");
            free(context);
            refused++;
            continue;
        }
        log_locked(runtime, "JOB_SUBMITTED", &runtime->jobs[job_index], "active application");
        accepted++;
    }
    pthread_mutex_unlock(&runtime->mutex);
    if (submitted != NULL)
        *submitted = accepted;
    if (rejected != NULL)
        *rejected = refused;
    return 0;
}

int application_runtime_submit_active(application_runtime_t *runtime,
                                      size_t *submitted, size_t *rejected)
{
    application_manager_record_t *records;
    size_t count;
    int result;

    if (runtime == NULL || !runtime->initialized)
        return EINVAL;
    count = application_manager_active_count(runtime->manager);
    records = calloc(count == 0 ? 1 : count, sizeof(*records));
    if (records == NULL)
        return ENOMEM;
    count = application_manager_snapshot(runtime->manager, records, count);
    result = application_runtime_submit_snapshot(runtime, records, count, submitted, rejected);
    free(records);
    return result;
}

int application_runtime_wait_idle(application_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->initialized)
        return EINVAL;
    return worker_pool_wait_idle(runtime->pool) == WORKER_POOL_SUCCESS ? 0 : EIO;
}

size_t application_runtime_snapshot(const application_runtime_t *runtime,
                                    application_runtime_job_record_t *records,
                                    size_t capacity)
{
    size_t count;

    if (runtime == NULL || records == NULL || capacity == 0 || !runtime->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&runtime->mutex);
    count = runtime->count < capacity ? runtime->count : capacity;
    for (size_t index = 0; index < count; index++)
        records[index] = runtime->jobs[index].record;
    pthread_mutex_unlock((pthread_mutex_t *)&runtime->mutex);
    return count;
}

size_t application_runtime_completed_count(const application_runtime_t *runtime)
{
    size_t count = 0;

    if (runtime == NULL || !runtime->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&runtime->mutex);
    for (size_t index = 0; index < runtime->count; index++)
        if (runtime->jobs[index].record.status == APPLICATION_RUNTIME_JOB_COMPLETED ||
            runtime->jobs[index].record.status == APPLICATION_RUNTIME_JOB_SKIPPED)
            count++;
    pthread_mutex_unlock((pthread_mutex_t *)&runtime->mutex);
    return count;
}

void application_runtime_shutdown(application_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->initialized)
        return;
    pthread_mutex_lock(&runtime->mutex);
    runtime->shutting_down = true;
    pthread_mutex_unlock(&runtime->mutex);
    worker_pool_wait_idle(runtime->pool);
    if (runtime->results_file != NULL) {
        fclose(runtime->results_file);
        runtime->results_file = NULL;
    }
    if (runtime->log_file != NULL) {
        fclose(runtime->log_file);
        runtime->log_file = NULL;
    }
    runtime->initialized = false;
}

void application_runtime_destroy(application_runtime_t *runtime)
{
    if (runtime == NULL)
        return;
    if (runtime->initialized)
        application_runtime_shutdown(runtime);
    free(runtime->jobs);
    pthread_mutex_destroy(&runtime->mutex);
    free(runtime);
}
