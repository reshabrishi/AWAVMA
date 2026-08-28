#define _POSIX_C_SOURCE 200809L

#include "application_manager.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_MAX_APPLICATIONS 1024U
#define DEFAULT_LOG_PATH "logs/application_manager.log"
#define DEFAULT_RESULTS_PATH "results/application_manager_results.csv"
#define DEFAULT_TABLE_PATH "results/application_table.csv"
#define RESULTS_HEADER "test_id,expected,actual,status,reason"
#define TABLE_HEADER "app_id,pid,parent_pid,process_name,executable_path,start_time_ticks,discovered_at,last_seen,state,status"

typedef struct {
    application_manager_record_t record;
    uint64_t last_seen_monotonic_ms;
    bool seen_in_scan;
} application_manager_entry_t;

struct application_manager {
    application_manager_config_t config;
    application_manager_entry_t *entries;
    size_t count;
    size_t capacity;
    pthread_mutex_t mutex;
    FILE *log_file;
    FILE *results_file;
    bool mutex_initialized;
    bool initialized;
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static const char *status_name(application_manager_status_t status)
{
    switch (status) {
    case APPLICATION_MANAGER_DISCOVERED: return "DISCOVERED";
    case APPLICATION_MANAGER_ACTIVE: return "ACTIVE";
    case APPLICATION_MANAGER_TERMINATED: return "TERMINATED";
    case APPLICATION_MANAGER_EXPIRED: return "EXPIRED";
    default: return "UNKNOWN";
    }
}

const char *application_manager_status_name(application_manager_status_t status)
{
    return status_name(status);
}

static void write_result_locked(application_manager_t *manager, const char *test_id,
                                const char *expected, const application_manager_entry_t *entry,
                                const char *reason)
{
    if (manager->results_file == NULL)
        return;
    fprintf(manager->results_file, "%s,%s,%s,%s,%s\n", test_id, expected,
            entry == NULL ? "-" : entry->record.app_id,
            entry == NULL ? "-" : status_name(entry->record.status),
            reason == NULL ? "-" : reason);
    fflush(manager->results_file);
}

static void log_event_locked(application_manager_t *manager, const char *event,
                             const application_manager_entry_t *entry,
                             const char *detail)
{
    if (manager->log_file == NULL)
        return;
    fprintf(manager->log_file, "%lld,%s,%s,%ld,%llu,%s,%s\n",
            (long long)time(NULL), event,
            entry == NULL ? "-" : entry->record.app_id,
            entry == NULL ? -1L : (long)entry->record.pid,
            entry == NULL ? 0ULL : (unsigned long long)entry->record.start_time_ticks,
            entry == NULL ? "-" : status_name(entry->record.status),
            detail == NULL ? "-" : detail);
    fflush(manager->log_file);
    write_result_locked(manager, event, status_name(entry == NULL ? APPLICATION_MANAGER_ACTIVE : entry->record.status), entry, detail);
}

static int ensure_results_header(FILE *file)
{
    long position;

    if (fseek(file, 0, SEEK_END) != 0 || (position = ftell(file)) < 0)
        return -1;
    if (position == 0 && fprintf(file, "%s\n", RESULTS_HEADER) < 0)
        return -1;
    return fflush(file) == 0 ? 0 : -1;
}

static int ensure_table_header(const char *path)
{
    FILE *file;
    long position;

    if (path == NULL)
        return 0;
    file = fopen(path, "a+");
    if (file == NULL)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0 || (position = ftell(file)) < 0 ||
        (position == 0 && fprintf(file, "%s\n", TABLE_HEADER) < 0) || fclose(file) != 0)
        return -1;
    return 0;
}

static void write_table_locked(const application_manager_t *manager)
{
    FILE *file;

    if (manager->config.table_path == NULL)
        return;
    file = fopen(manager->config.table_path, "w");
    if (file == NULL)
        return;
    fprintf(file, "%s\n", TABLE_HEADER);
    for (size_t index = 0; index < manager->count; index++) {
        const application_manager_record_t *record = &manager->entries[index].record;

        fprintf(file, "%s,%ld,%ld,%s,%s,%llu,%lld,%lld,%c,%s\n",
                record->app_id, (long)record->pid, (long)record->parent_pid,
                record->process_name,
                record->executable_path[0] == '\0' ? "-" : record->executable_path,
                (unsigned long long)record->start_time_ticks,
                (long long)record->discovered_at, (long long)record->last_seen,
                record->state == '\0' ? '?' : record->state,
                status_name(record->status));
    }
    fclose(file);
}

static ssize_t identity_index_locked(const application_manager_t *manager,
                                     pid_t pid, uint64_t start_time_ticks)
{
    for (size_t index = 0; index < manager->count; index++)
        if (manager->entries[index].record.pid == pid &&
            manager->entries[index].record.start_time_ticks == start_time_ticks)
            return (ssize_t)index;
    return -1;
}

static int append_entry_locked(application_manager_t *manager,
                               const application_discovery_record_t *discovery,
                               time_t now, uint64_t now_monotonic)
{
    application_manager_entry_t *expanded;
    application_manager_entry_t *entry;
    size_t capacity;

    if (manager->count >= manager->config.max_applications)
        return ENOSPC;
    if (manager->count == manager->capacity) {
        capacity = manager->capacity == 0 ? 32U : manager->capacity * 2U;
        if (capacity > manager->config.max_applications)
            capacity = manager->config.max_applications;
        expanded = realloc(manager->entries, capacity * sizeof(*expanded));
        if (expanded == NULL)
            return ENOMEM;
        manager->entries = expanded;
        manager->capacity = capacity;
    }
    entry = &manager->entries[manager->count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->record.app_id, sizeof(entry->record.app_id), "APP_%ld_%llu",
             (long)discovery->pid, (unsigned long long)discovery->start_time_ticks);
    entry->record.pid = discovery->pid;
    entry->record.parent_pid = discovery->parent_pid;
    entry->record.start_time_ticks = discovery->start_time_ticks;
    entry->record.state = discovery->state;
    snprintf(entry->record.process_name, sizeof(entry->record.process_name), "%s",
             discovery->process_name);
    snprintf(entry->record.executable_path, sizeof(entry->record.executable_path), "%s",
             discovery->executable_path);
    entry->record.discovered_at = now;
    entry->record.last_seen = now;
    entry->record.status = APPLICATION_MANAGER_DISCOVERED;
    entry->last_seen_monotonic_ms = now_monotonic;
    entry->seen_in_scan = true;
    log_event_locked(manager, "DISCOVERED", entry, "new identity");
    log_event_locked(manager, "REGISTERED", entry, "application table");
    entry->record.status = APPLICATION_MANAGER_ACTIVE;
    log_event_locked(manager, "ACTIVE", entry, "observed");
    return 0;
}

static void update_entry_locked(application_manager_t *manager,
                                application_manager_entry_t *entry,
                                const application_discovery_record_t *discovery,
                                time_t now, uint64_t now_monotonic)
{
    entry->record.parent_pid = discovery->parent_pid;
    entry->record.state = discovery->state;
    snprintf(entry->record.process_name, sizeof(entry->record.process_name), "%s",
             discovery->process_name);
    snprintf(entry->record.executable_path, sizeof(entry->record.executable_path), "%s",
             discovery->executable_path);
    entry->record.last_seen = now;
    entry->last_seen_monotonic_ms = now_monotonic;
    entry->seen_in_scan = true;
    if (entry->record.status != APPLICATION_MANAGER_ACTIVE) {
        entry->record.status = APPLICATION_MANAGER_ACTIVE;
        log_event_locked(manager, "ACTIVE", entry, "identity observed again");
    }
}

static void activate_entry_locked(application_manager_t *manager,
                                  application_manager_entry_t *entry)
{
    if (entry->record.status == APPLICATION_MANAGER_ACTIVE)
        return;
    entry->record.status = APPLICATION_MANAGER_ACTIVE;
    log_event_locked(manager, "ACTIVE", entry, "identity observed again");
}

static void terminate_entry_locked(application_manager_t *manager,
                                   application_manager_entry_t *entry,
                                   const char *reason)
{
    if (entry->record.status == APPLICATION_MANAGER_TERMINATED ||
        entry->record.status == APPLICATION_MANAGER_EXPIRED)
        return;
    entry->record.status = APPLICATION_MANAGER_TERMINATED;
    log_event_locked(manager, "TERMINATED", entry, reason);
}

void application_manager_config_default(application_manager_config_t *config)
{
    if (config == NULL)
        return;
    config->max_applications = DEFAULT_MAX_APPLICATIONS;
    config->application_idle_timeout_ms = 0;
    config->log_path = DEFAULT_LOG_PATH;
    config->results_path = DEFAULT_RESULTS_PATH;
    config->table_path = DEFAULT_TABLE_PATH;
}

application_manager_t *application_manager_create(void)
{
    return calloc(1, sizeof(application_manager_t));
}

int application_manager_init(application_manager_t *manager,
                             const application_manager_config_t *config)
{
    application_manager_config_t defaults;

    if (manager == NULL)
        return EINVAL;
    if (config == NULL) {
        application_manager_config_default(&defaults);
        config = &defaults;
    }
    if (config->max_applications == 0)
        return EINVAL;
    memset(manager, 0, sizeof(*manager));
    manager->config = *config;
    if (pthread_mutex_init(&manager->mutex, NULL) != 0)
        return EINVAL;
    manager->mutex_initialized = true;
    manager->entries = calloc(config->max_applications, sizeof(*manager->entries));
    if (manager->entries == NULL)
        goto fail;
    manager->capacity = config->max_applications;
    if (config->log_path != NULL) {
        manager->log_file = fopen(config->log_path, "a");
        if (manager->log_file == NULL)
            goto fail;
    }
    if (config->results_path != NULL) {
        manager->results_file = fopen(config->results_path, "a+");
        if (manager->results_file == NULL || ensure_results_header(manager->results_file) != 0)
            goto fail;
    }
    if (ensure_table_header(config->table_path) != 0)
        goto fail;
    manager->initialized = true;
    return 0;

fail:
    if (manager->results_file != NULL)
        fclose(manager->results_file);
    if (manager->log_file != NULL)
        fclose(manager->log_file);
    free(manager->entries);
    manager->entries = NULL;
    pthread_mutex_destroy(&manager->mutex);
    manager->mutex_initialized = false;
    return errno == 0 ? EIO : errno;
}

int application_manager_process_snapshot(application_manager_t *manager,
                                         const application_discovery_record_t *records,
                                         size_t count)
{
    time_t now;
    uint64_t now_monotonic;

    if (manager == NULL || !manager->initialized || (count > 0 && records == NULL))
        return EINVAL;
    now = time(NULL);
    now_monotonic = monotonic_ms();
    pthread_mutex_lock(&manager->mutex);
    for (size_t index = 0; index < manager->count; index++)
        manager->entries[index].seen_in_scan = false;
    for (size_t index = 0; index < count; index++) {
        ssize_t found = identity_index_locked(manager, records[index].pid,
                                              records[index].start_time_ticks);

        if (found >= 0) {
            application_manager_entry_t *entry = &manager->entries[found];

            update_entry_locked(manager, entry, &records[index], now, now_monotonic);
            activate_entry_locked(manager, entry);
            continue;
        }
        for (size_t old = 0; old < manager->count; old++) {
            application_manager_entry_t *entry = &manager->entries[old];

            if (entry->record.pid == records[index].pid &&
                entry->record.start_time_ticks != records[index].start_time_ticks) {
                log_event_locked(manager, "PID_REUSE", entry, "new start time detected");
                terminate_entry_locked(manager, entry, "PID reused");
            }
        }
        if (append_entry_locked(manager, &records[index], now, now_monotonic) != 0) {
            pthread_mutex_unlock(&manager->mutex);
            return ENOSPC;
        }
    }
    for (size_t index = 0; index < manager->count; index++) {
        application_manager_entry_t *entry = &manager->entries[index];

        if (!entry->seen_in_scan &&
            (entry->record.status == APPLICATION_MANAGER_ACTIVE ||
             entry->record.status == APPLICATION_MANAGER_DISCOVERED))
            terminate_entry_locked(manager, entry, "absent from discovery snapshot");
        if (entry->record.status == APPLICATION_MANAGER_TERMINATED &&
            manager->config.application_idle_timeout_ms > 0 &&
            now_monotonic - entry->last_seen_monotonic_ms >= manager->config.application_idle_timeout_ms) {
            entry->record.status = APPLICATION_MANAGER_EXPIRED;
            log_event_locked(manager, "EXPIRED", entry, "idle timeout");
        }
    }
    write_table_locked(manager);
    pthread_mutex_unlock(&manager->mutex);
    return 0;
}

bool application_manager_lookup_app_id(const application_manager_t *manager,
                                       const char *app_id,
                                       application_manager_record_t *record)
{
    bool found = false;

    if (manager == NULL || app_id == NULL || record == NULL || !manager->initialized)
        return false;
    pthread_mutex_lock((pthread_mutex_t *)&manager->mutex);
    for (size_t index = 0; index < manager->count; index++) {
        if (strcmp(manager->entries[index].record.app_id, app_id) == 0) {
            *record = manager->entries[index].record;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&manager->mutex);
    return found;
}

bool application_manager_lookup_identity(const application_manager_t *manager,
                                         pid_t pid, uint64_t start_time_ticks,
                                         application_manager_record_t *record)
{
    ssize_t index;
    bool found;

    if (manager == NULL || record == NULL || !manager->initialized)
        return false;
    pthread_mutex_lock((pthread_mutex_t *)&manager->mutex);
    index = identity_index_locked(manager, pid, start_time_ticks);
    found = index >= 0;
    if (found)
        *record = manager->entries[index].record;
    pthread_mutex_unlock((pthread_mutex_t *)&manager->mutex);
    return found;
}

size_t application_manager_active_count(const application_manager_t *manager)
{
    size_t count = 0;

    if (manager == NULL || !manager->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&manager->mutex);
    for (size_t index = 0; index < manager->count; index++)
        if (manager->entries[index].record.status == APPLICATION_MANAGER_ACTIVE)
            count++;
    pthread_mutex_unlock((pthread_mutex_t *)&manager->mutex);
    return count;
}

size_t application_manager_snapshot(const application_manager_t *manager,
                                   application_manager_record_t *records,
                                   size_t capacity)
{
    size_t copied = 0;

    if (manager == NULL || records == NULL || capacity == 0 || !manager->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&manager->mutex);
    for (size_t index = 0; index < manager->count && copied < capacity; index++)
        if (manager->entries[index].record.status == APPLICATION_MANAGER_ACTIVE)
            records[copied++] = manager->entries[index].record;
    pthread_mutex_unlock((pthread_mutex_t *)&manager->mutex);
    return copied;
}

void application_manager_shutdown(application_manager_t *manager)
{
    if (manager == NULL || !manager->initialized)
        return;
    pthread_mutex_lock(&manager->mutex);
    write_table_locked(manager);
    log_event_locked(manager, "SHUTDOWN", NULL, "manager stopped");
    if (manager->results_file != NULL) {
        fclose(manager->results_file);
        manager->results_file = NULL;
    }
    if (manager->log_file != NULL) {
        fclose(manager->log_file);
        manager->log_file = NULL;
    }
    manager->initialized = false;
    pthread_mutex_unlock(&manager->mutex);
}

void application_manager_destroy(application_manager_t *manager)
{
    if (manager == NULL)
        return;
    if (manager->initialized)
        application_manager_shutdown(manager);
    if (manager->mutex_initialized)
        pthread_mutex_destroy(&manager->mutex);
    free(manager->entries);
    free(manager);
}
