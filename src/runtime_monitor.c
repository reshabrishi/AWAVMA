#define _POSIX_C_SOURCE 200809L

#include "runtime_monitor.h"
#include "monitor.h"
#include "monitor_profile.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DEFAULT_INTERVAL_MS 100U
#define DEFAULT_MAX_APPLICATIONS 256U
#define DEFAULT_RESULTS_DIR "results/continuous_monitoring"
#define DEFAULT_RESULTS_PATH "results/continuous_monitoring_results.csv"
#define DEFAULT_LOG_PATH "logs/continuous_monitoring.log"
#define RUNTIME_RESULTS_HEADER "app_id,pid,start_time_ticks,worker_id,status,samples,submissions,deferred,last_result"

typedef struct {
    runtime_monitor_record_t record;
    application_manager_record_t application;
    uint64_t next_due_ms;
    uint64_t next_due_ns;
    bool seen;
} monitor_entry_t;

struct runtime_monitor {
    application_manager_t *manager;
    worker_pool_t *pool;
    application_discovery_t *discovery;
    runtime_monitor_config_t config;
    monitor_entry_t *entries;
    size_t count;
    size_t peak_in_flight;
    size_t active_jobs;
    size_t peak_active_jobs;
    size_t deferred_count;
    runtime_monitor_discovery_stats_t discovery_stats;
    bool have_discovery_snapshot;
    pthread_mutex_t mutex;
    FILE *results_file;
    FILE *log_file;
    bool mutex_initialized;
    bool initialized;
    _Atomic bool stop_requested;
};

typedef struct {
    runtime_monitor_t *monitor;
    size_t entry_index;
    uint64_t job_id;
    uint64_t submitted_ns;
} monitor_job_context_t;

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {milliseconds / 1000U,
                             (long)(milliseconds % 1000U) * 1000000L};

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static const char *status_name(runtime_monitor_status_t status)
{
    switch (status) {
    case RUNTIME_MONITOR_ACTIVE: return "ACTIVE";
    case RUNTIME_MONITOR_TERMINATED: return "TERMINATED";
    case RUNTIME_MONITOR_TARGET_GONE: return "TARGET_GONE";
    case RUNTIME_MONITOR_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

const char *runtime_monitor_status_name(runtime_monitor_status_t status)
{
    return status_name(status);
}

static void log_locked(runtime_monitor_t *monitor, const char *event,
                       const monitor_entry_t *entry, const char *detail)
{
    if (monitor->log_file == NULL)
        return;
    fprintf(monitor->log_file, "%lld,event=%s,app_id=%s,pid=%ld,start_time=%llu,status=%s,detail=%s\n",
            (long long)time(NULL), event, entry->record.app_id, (long)entry->record.pid,
            (unsigned long long)entry->record.start_time_ticks,
            status_name(entry->record.status), detail == NULL ? "-" : detail);
    fflush(monitor->log_file);
}

static void write_result_locked(runtime_monitor_t *monitor, const monitor_entry_t *entry)
{
    if (monitor->results_file == NULL)
        return;
    fprintf(monitor->results_file, "%s,%ld,%llu,%d,%s,%zu,%zu,%zu,%d\n",
            entry->record.app_id, (long)entry->record.pid,
            (unsigned long long)entry->record.start_time_ticks,
            entry->record.last_worker_id, status_name(entry->record.status),
            entry->record.samples, entry->record.submissions,
            entry->record.deferred_jobs, entry->record.last_result);
    fflush(monitor->results_file);
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

static ssize_t find_entry_locked(const runtime_monitor_t *monitor,
                                 const application_manager_record_t *application)
{
    for (size_t index = 0; index < monitor->count; index++) {
        const runtime_monitor_record_t *record = &monitor->entries[index].record;

        if (record->pid == application->pid &&
            record->start_time_ticks == application->start_time_ticks &&
            strcmp(record->app_id, application->app_id) == 0)
            return (ssize_t)index;
    }
    return -1;
}

static ssize_t append_entry_locked(runtime_monitor_t *monitor,
                                   const application_manager_record_t *application,
                                   uint64_t now)
{
    monitor_entry_t *entry;

    if (monitor->count >= monitor->config.max_applications)
        return -1;
    entry = &monitor->entries[monitor->count++];
    memset(entry, 0, sizeof(*entry));
    entry->application = *application;
    snprintf(entry->record.app_id, sizeof(entry->record.app_id), "%s", application->app_id);
    entry->record.pid = application->pid;
    entry->record.start_time_ticks = application->start_time_ticks;
    entry->record.status = RUNTIME_MONITOR_ACTIVE;
    entry->record.last_worker_id = -1;
    entry->next_due_ms = now;
#ifdef AWAVMA_PROFILE
    entry->next_due_ns = monitor_profile_now_ns();
#endif
    monitor->discovery_stats.new_applications_found++;
#ifdef AWAVMA_PROFILE
    monitor_profile_counter("discovery", "new_applications_found", entry->record.app_id,
                            entry->record.pid, 0, 1, "DISCOVERED");
#endif
    return (ssize_t)(monitor->count - 1);
}

static int make_output_path(char *buffer, size_t size, const char *directory,
                            const char *app_id, const char *suffix)
{
    int written = snprintf(buffer, size, "%s/%s_%s.csv", directory, app_id, suffix);

    return written < 0 || (size_t)written >= size ? -1 : 0;
}

static void monitor_job(void *argument)
{
    monitor_job_context_t *context = argument;
    runtime_monitor_t *monitor = context->monitor;
    application_manager_record_t application;
    monitor_entry_t *entry;
    monitor_config_t config;
    char output_path[4096];
    char thread_output_path[4096];
    bool valid;
    int result;
    int worker_id;
    monitor_profile_scope_t event_profile;
    monitor_profile_scope_t execution_profile;
    monitor_profile_scope_t worker_profile;

    pthread_mutex_lock(&monitor->mutex);
    entry = &monitor->entries[context->entry_index];
    application = entry->application;
    worker_id = identify_worker(monitor->pool, context->job_id);
    entry->record.last_worker_id = worker_id;
    monitor->active_jobs++;
    if (monitor->active_jobs > monitor->peak_active_jobs)
        monitor->peak_active_jobs = monitor->active_jobs;
    pthread_mutex_unlock(&monitor->mutex);

#ifdef AWAVMA_PROFILE
    if (worker_id >= 0)
        monitor_profile_counter("worker", "worker_assignment", application.app_id,
                                application.pid, context->job_id, (uint64_t)worker_id, "ASSIGNED");
    monitor_profile_event("worker", "queue_wait", application.app_id, application.pid,
                          context->job_id, context->submitted_ns, monitor_profile_now_ns(), "OK");
#endif
    monitor_profile_scope_begin(&event_profile, "worker", "job_started",
                                application.app_id, application.pid, context->job_id);
    monitor_profile_scope_end(&event_profile, "OK");
    monitor_profile_scope_begin(&execution_profile, "worker", "monitor_execution",
                                 application.app_id, application.pid, context->job_id);
    monitor_profile_scope_begin(&worker_profile, "worker", "worker_execution",
                                 application.app_id, application.pid, context->job_id);

    valid = application_manager_lookup_identity(monitor->manager, application.pid,
                                                application.start_time_ticks,
                                                &application) &&
            application.status == APPLICATION_MANAGER_ACTIVE;
    result = -1;
    if (valid && make_output_path(output_path, sizeof(output_path), monitor->config.results_dir,
                                 application.app_id, "monitoring") == 0 &&
        make_output_path(thread_output_path, sizeof(thread_output_path), monitor->config.results_dir,
                         application.app_id, "threads") == 0) {
        memset(&config, 0, sizeof(config));
        config.pid = application.pid;
        config.expected_start_time_ticks = application.start_time_ticks;
        config.expected_start_time_available = true;
        config.interval_ms = (unsigned)monitor->config.monitor_interval_ms;
        config.output_path = output_path;
        config.thread_output_path = thread_output_path;
        config.log_path = NULL;
        config.target_is_child = false;
        config.stop_requested = NULL;
        result = monitor_run_pid_once(&config);
    }
    monitor_profile_scope_end(&execution_profile, result == 0 ? "OK" : "ERROR");
    monitor_profile_scope_end(&worker_profile, result == 0 ? "OK" : "ERROR");

    pthread_mutex_lock(&monitor->mutex);
    entry = &monitor->entries[context->entry_index];
    entry->record.in_flight = false;
    entry->record.last_result = result;
    monitor->active_jobs--;
    if (!valid || result != 0) {
        bool target_gone = !valid || result == MONITOR_RESULT_TARGET_GONE;

        entry->record.status = target_gone ? RUNTIME_MONITOR_TARGET_GONE : RUNTIME_MONITOR_ERROR;
        log_locked(monitor, target_gone ? "TARGET_GONE" : "SAMPLE_ERROR", entry,
                    target_gone ? "application identity no longer matches" : "Phase 3 one-sample call failed");
    } else {
        entry->record.status = RUNTIME_MONITOR_ACTIVE;
        entry->record.samples++;
        log_locked(monitor, "SAMPLE_COMPLETED", entry, "Phase 3 sample written");
    }
    write_result_locked(monitor, entry);
    pthread_mutex_unlock(&monitor->mutex);
    monitor_profile_scope_begin(&event_profile, "worker", "job_completed",
                                application.app_id, application.pid, context->job_id);
    monitor_profile_scope_end(&event_profile, result == 0 ? "OK" : "ERROR");
    free(context);
}

static int schedule_active(runtime_monitor_t *monitor,
                           const application_manager_record_t *active,
                           size_t count, uint64_t now)
{
    pthread_mutex_lock(&monitor->mutex);
    for (size_t index = 0; index < monitor->count; index++)
        monitor->entries[index].seen = false;
    for (size_t index = 0; index < count; index++) {
        ssize_t entry_index = find_entry_locked(monitor, &active[index]);

        if (entry_index < 0)
            entry_index = append_entry_locked(monitor, &active[index], now);
        if (entry_index < 0)
            continue;
        monitor_entry_t *entry = &monitor->entries[entry_index];

        entry->application = active[index];
        /* A gone identity cannot return; PID reuse receives a new start-time key. */
        if (entry->record.status == RUNTIME_MONITOR_TARGET_GONE ||
            entry->record.status == RUNTIME_MONITOR_TERMINATED) {
            entry->seen = true;
            continue;
        }
        entry->record.status = RUNTIME_MONITOR_ACTIVE;
        entry->seen = true;
        if (entry->record.in_flight || now < entry->next_due_ms)
            continue;
        monitor_job_context_t *context = malloc(sizeof(*context));

        if (context == NULL)
            continue;
        context->monitor = monitor;
        context->entry_index = (size_t)entry_index;
        context->job_id = ((uint64_t)(entry_index + 1) << 32) ^
                           (uint64_t)(entry->record.submissions + 1);
        worker_job_t job = {monitor_job, context, context->job_id};
        monitor_profile_scope_t submission_profile;

#ifdef AWAVMA_PROFILE
        context->submitted_ns = monitor_profile_now_ns();
        if (context->submitted_ns >= entry->next_due_ns)
            monitor_profile_event("runtime", "monitor_eligibility_wait", entry->record.app_id,
                                  entry->record.pid, context->job_id, entry->next_due_ns,
                                  context->submitted_ns, "OK");
        monitor_profile_counter("worker", "queue_depth", entry->record.app_id,
                                entry->record.pid, context->job_id,
                                worker_pool_queued(monitor->pool), "OBSERVED");
#endif
        monitor_profile_scope_begin(&submission_profile, "worker", "job_submission",
                                    entry->record.app_id, entry->record.pid, context->job_id);
        worker_pool_status_t result = worker_pool_submit(monitor->pool, &job);
        monitor_profile_scope_end(&submission_profile,
                                  result == WORKER_POOL_SUCCESS ? "OK" : "DEFERRED");

        if (result == WORKER_POOL_SUCCESS) {
            entry->record.in_flight = true;
            entry->record.submissions++;
            entry->next_due_ms = now + monitor->config.monitor_interval_ms;
#ifdef AWAVMA_PROFILE
            entry->next_due_ns = context->submitted_ns +
                                 monitor->config.monitor_interval_ms * 1000000ULL;
#endif
            size_t in_flight = 0;

            for (size_t active_index = 0; active_index < monitor->count; active_index++)
                if (monitor->entries[active_index].record.in_flight)
                    in_flight++;
            if (in_flight > monitor->peak_in_flight)
                monitor->peak_in_flight = in_flight;
            log_locked(monitor, "SAMPLE_SUBMITTED", entry, "scheduled");
        } else {
#ifdef AWAVMA_PROFILE
            monitor_profile_counter("worker", "queue_deferred", entry->record.app_id,
                                    entry->record.pid, context->job_id, 1,
                                    worker_pool_status_name(result));
            if (result == WORKER_POOL_QUEUE_FULL)
                monitor_profile_counter("worker", "queue_saturation", entry->record.app_id,
                                        entry->record.pid, context->job_id, 1,
                                        worker_pool_status_name(result));
#endif
            entry->record.deferred_jobs++;
            monitor->deferred_count++;
            entry->next_due_ms = now + monitor->config.monitor_interval_ms;
            log_locked(monitor, "SAMPLE_DEFERRED", entry,
                       worker_pool_status_name(result));
            free(context);
        }
    }
    for (size_t index = 0; index < monitor->count; index++) {
        monitor_entry_t *entry = &monitor->entries[index];

        if (!entry->seen && !entry->record.in_flight &&
            entry->record.status == RUNTIME_MONITOR_ACTIVE) {
            entry->record.status = RUNTIME_MONITOR_TERMINATED;
            monitor->discovery_stats.applications_terminated++;
#ifdef AWAVMA_PROFILE
            monitor_profile_counter("discovery", "applications_terminated", entry->record.app_id,
                                    entry->record.pid, 0, 1, "TERMINATED");
#endif
            log_locked(monitor, "APPLICATION_TERMINATED", entry, "absent from manager snapshot");
            write_result_locked(monitor, entry);
        }
    }
    pthread_mutex_unlock(&monitor->mutex);
    return 0;
}

void runtime_monitor_config_default(runtime_monitor_config_t *config)
{
    if (config == NULL)
        return;
    config->monitor_interval_ms = DEFAULT_INTERVAL_MS;
    config->discovery_interval_ms = 250;
    config->max_applications = DEFAULT_MAX_APPLICATIONS;
    config->results_dir = DEFAULT_RESULTS_DIR;
    config->results_path = DEFAULT_RESULTS_PATH;
    config->log_path = DEFAULT_LOG_PATH;
    application_discovery_config_default(&config->discovery_config);
    config->application_filter = NULL;
    config->application_filter_context = NULL;
}

runtime_monitor_t *runtime_monitor_create(void)
{
    return calloc(1, sizeof(runtime_monitor_t));
}

int runtime_monitor_init(runtime_monitor_t *monitor,
                         application_manager_t *manager,
                         worker_pool_t *pool,
                         const runtime_monitor_config_t *config)
{
    runtime_monitor_config_t defaults;

    if (monitor == NULL || manager == NULL || pool == NULL)
        return EINVAL;
    if (config == NULL) {
        runtime_monitor_config_default(&defaults);
        config = &defaults;
    }
    if (config->monitor_interval_ms == 0 || config->max_applications == 0 ||
        config->results_dir == NULL)
        return EINVAL;
    memset(monitor, 0, sizeof(*monitor));
    monitor->manager = manager;
    monitor->pool = pool;
    monitor->config = *config;
    monitor->entries = calloc(config->max_applications, sizeof(*monitor->entries));
    monitor->discovery = application_discovery_create();
    if (monitor->entries == NULL || monitor->discovery == NULL)
        goto fail;
    if (mkdir(config->results_dir, 0755) != 0 && errno != EEXIST)
        goto fail;
    if (pthread_mutex_init(&monitor->mutex, NULL) != 0)
        goto fail;
    monitor->mutex_initialized = true;
    if (application_discovery_init(monitor->discovery, &config->discovery_config) != 0)
        goto fail_mutex;
    if (config->results_path != NULL) {
        monitor->results_file = fopen(config->results_path, "w");
        if (monitor->results_file == NULL)
            goto fail_mutex;
        fprintf(monitor->results_file, "%s\n", RUNTIME_RESULTS_HEADER);
    }
    if (config->log_path != NULL) {
        monitor->log_file = fopen(config->log_path, "a");
        if (monitor->log_file == NULL)
            goto fail_mutex;
    }
    monitor->initialized = true;
    return 0;

fail_mutex:
    if (monitor->results_file != NULL)
        fclose(monitor->results_file);
    if (monitor->log_file != NULL)
        fclose(monitor->log_file);
    if (monitor->mutex_initialized) {
        pthread_mutex_destroy(&monitor->mutex);
        monitor->mutex_initialized = false;
    }
fail:
    application_discovery_destroy(monitor->discovery);
    free(monitor->entries);
    monitor->discovery = NULL;
    monitor->entries = NULL;
    return EIO;
}

static int refresh_discovery(runtime_monitor_t *monitor, uint64_t now)
{
    application_discovery_record_t *discovered;
    size_t discovered_count;

    pthread_mutex_lock(&monitor->mutex);
    monitor->discovery_stats.last_discovery_started_ms = now;
    pthread_mutex_unlock(&monitor->mutex);
    if (application_discovery_scan(monitor->discovery) != 0)
        return EIO;
    discovered_count = application_discovery_count(monitor->discovery);
    discovered = calloc(discovered_count == 0 ? 1 : discovered_count, sizeof(*discovered));
    if (discovered == NULL)
        return ENOMEM;
    for (size_t index = 0; index < discovered_count; index++)
        if (!application_discovery_get(monitor->discovery, index, &discovered[index])) {
            free(discovered);
            return EIO;
        }
    if (application_manager_process_snapshot(monitor->manager, discovered, discovered_count) != 0) {
        free(discovered);
        return EIO;
    }
    free(discovered);
    pthread_mutex_lock(&monitor->mutex);
    monitor->have_discovery_snapshot = true;
    monitor->discovery_stats.discovery_scan_count++;
    monitor->discovery_stats.last_discovery_completed_ms = monotonic_ms();
    monitor->discovery_stats.next_discovery_due_ms =
        monitor->config.discovery_interval_ms == 0 ? now : now + monitor->config.discovery_interval_ms;
    pthread_mutex_unlock(&monitor->mutex);
#ifdef AWAVMA_PROFILE
    monitor_profile_counter("discovery", "discovery_scan_count", NULL, -1, 0, 1, "SCANNED");
#endif
    return 0;
}

int runtime_monitor_run_for(runtime_monitor_t *monitor, uint64_t duration_ms)
{
    uint64_t deadline;

    if (monitor == NULL || !monitor->initialized || duration_ms == 0)
        return EINVAL;
    deadline = monotonic_ms() + duration_ms;
    while (monotonic_ms() < deadline) {
        application_manager_record_t *active;
        size_t active_count;
        uint64_t now = monotonic_ms();
        monitor_profile_scope_t schedule_profile;
        bool discovery_due;

        if (atomic_load(&monitor->stop_requested))
            break;
        pthread_mutex_lock(&monitor->mutex);
        discovery_due = !monitor->have_discovery_snapshot ||
                        monitor->config.discovery_interval_ms == 0 ||
                        now >= monitor->discovery_stats.next_discovery_due_ms;
        if (!discovery_due)
            monitor->discovery_stats.discovery_skipped_not_due_count++;
        pthread_mutex_unlock(&monitor->mutex);
        if (discovery_due) {
            int discovery_result = refresh_discovery(monitor, now);

            if (discovery_result != 0)
                return discovery_result;
        } else {
#ifdef AWAVMA_PROFILE
            monitor_profile_counter("discovery", "discovery_skipped_not_due_count", NULL, -1, 0, 1,
                                    "SKIPPED");
#endif
        }
        active_count = application_manager_active_count(monitor->manager);
        active = calloc(active_count == 0 ? 1 : active_count, sizeof(*active));
        if (active == NULL)
            return ENOMEM;
        active_count = application_manager_snapshot(monitor->manager, active, active_count);
        if (monitor->config.application_filter != NULL) {
            size_t filtered_count = 0;

            for (size_t index = 0; index < active_count; index++)
                if (monitor->config.application_filter(&active[index],
                                                       monitor->config.application_filter_context))
                    active[filtered_count++] = active[index];
            active_count = filtered_count;
        }
        monitor_profile_scope_begin(&schedule_profile, "runtime", "runtime_schedule_active",
                                    NULL, -1, 0);
        schedule_active(monitor, active, active_count, now);
        monitor_profile_scope_end(&schedule_profile, "OK");
        pthread_mutex_lock(&monitor->mutex);
        monitor->discovery_stats.schedule_pass_count++;
        pthread_mutex_unlock(&monitor->mutex);
        free(active);
        sleep_ms((unsigned)(monitor->config.monitor_interval_ms / 4U + 1U));
    }
    return worker_pool_wait_idle(monitor->pool) == WORKER_POOL_SUCCESS ? 0 : EIO;
}

void runtime_monitor_request_stop(runtime_monitor_t *monitor)
{
    if (monitor != NULL)
        atomic_store(&monitor->stop_requested, true);
}

size_t runtime_monitor_snapshot(const runtime_monitor_t *monitor,
                                runtime_monitor_record_t *records,
                                size_t capacity)
{
    size_t count;

    if (monitor == NULL || records == NULL || capacity == 0 || !monitor->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&monitor->mutex);
    count = monitor->count < capacity ? monitor->count : capacity;
    for (size_t index = 0; index < count; index++)
        records[index] = monitor->entries[index].record;
    pthread_mutex_unlock((pthread_mutex_t *)&monitor->mutex);
    return count;
}

size_t runtime_monitor_peak_in_flight(const runtime_monitor_t *monitor)
{
    size_t value;

    if (monitor == NULL || !monitor->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&monitor->mutex);
    value = monitor->peak_active_jobs;
    pthread_mutex_unlock((pthread_mutex_t *)&monitor->mutex);
    return value;
}

size_t runtime_monitor_deferred_count(const runtime_monitor_t *monitor)
{
    size_t value;

    if (monitor == NULL || !monitor->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&monitor->mutex);
    value = monitor->deferred_count;
    pthread_mutex_unlock((pthread_mutex_t *)&monitor->mutex);
    return value;
}

bool runtime_monitor_discovery_stats(const runtime_monitor_t *monitor,
                                     runtime_monitor_discovery_stats_t *stats)
{
    if (monitor == NULL || stats == NULL || !monitor->initialized)
        return false;
    pthread_mutex_lock((pthread_mutex_t *)&monitor->mutex);
    *stats = monitor->discovery_stats;
    pthread_mutex_unlock((pthread_mutex_t *)&monitor->mutex);
    return true;
}

void runtime_monitor_shutdown(runtime_monitor_t *monitor)
{
    if (monitor == NULL || !monitor->initialized)
        return;
    atomic_store(&monitor->stop_requested, true);
    worker_pool_wait_idle(monitor->pool);
    if (monitor->results_file != NULL)
        fclose(monitor->results_file);
    if (monitor->log_file != NULL)
        fclose(monitor->log_file);
    monitor->results_file = NULL;
    monitor->log_file = NULL;
    monitor->initialized = false;
}

void runtime_monitor_destroy(runtime_monitor_t *monitor)
{
    if (monitor == NULL)
        return;
    if (monitor->initialized)
        runtime_monitor_shutdown(monitor);
    application_discovery_destroy(monitor->discovery);
    if (monitor->mutex_initialized)
        pthread_mutex_destroy(&monitor->mutex);
    free(monitor->entries);
    free(monitor);
}
