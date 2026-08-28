#ifndef AWAVMA_RUNTIME_MONITOR_H
#define AWAVMA_RUNTIME_MONITOR_H

#include "application_discovery.h"
#include "application_manager.h"
#include "worker_pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*runtime_monitor_application_filter_fn)(
    const application_manager_record_t *application, void *context);

typedef enum {
    RUNTIME_MONITOR_ACTIVE,
    RUNTIME_MONITOR_TERMINATED,
    RUNTIME_MONITOR_TARGET_GONE,
    RUNTIME_MONITOR_ERROR
} runtime_monitor_status_t;

typedef struct {
    uint64_t monitor_interval_ms;
    /* Zero preserves the legacy behavior of scanning on every scheduler pass. */
    uint64_t discovery_interval_ms;
    size_t max_applications;
    const char *results_dir;
    const char *results_path;
    const char *log_path;
    application_discovery_config_t discovery_config;
    runtime_monitor_application_filter_fn application_filter;
    void *application_filter_context;
} runtime_monitor_config_t;

typedef struct {
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    runtime_monitor_status_t status;
    bool in_flight;
    size_t submissions;
    size_t samples;
    size_t deferred_jobs;
    int last_result;
    int last_worker_id;
} runtime_monitor_record_t;

typedef struct {
    uint64_t discovery_scan_count;
    uint64_t discovery_skipped_not_due_count;
    uint64_t schedule_pass_count;
    uint64_t new_applications_found;
    uint64_t applications_terminated;
    uint64_t last_discovery_started_ms;
    uint64_t last_discovery_completed_ms;
    uint64_t next_discovery_due_ms;
} runtime_monitor_discovery_stats_t;

typedef struct runtime_monitor runtime_monitor_t;

void runtime_monitor_config_default(runtime_monitor_config_t *config);
const char *runtime_monitor_status_name(runtime_monitor_status_t status);
runtime_monitor_t *runtime_monitor_create(void);
int runtime_monitor_init(runtime_monitor_t *monitor,
                         application_manager_t *manager,
                         worker_pool_t *pool,
                         const runtime_monitor_config_t *config);
int runtime_monitor_run_for(runtime_monitor_t *monitor, uint64_t duration_ms);
void runtime_monitor_request_stop(runtime_monitor_t *monitor);
size_t runtime_monitor_snapshot(const runtime_monitor_t *monitor,
                                runtime_monitor_record_t *records,
                                size_t capacity);
size_t runtime_monitor_peak_in_flight(const runtime_monitor_t *monitor);
size_t runtime_monitor_deferred_count(const runtime_monitor_t *monitor);
bool runtime_monitor_discovery_stats(const runtime_monitor_t *monitor,
                                     runtime_monitor_discovery_stats_t *stats);
void runtime_monitor_shutdown(runtime_monitor_t *monitor);
void runtime_monitor_destroy(runtime_monitor_t *monitor);

#endif
