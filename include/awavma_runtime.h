#ifndef AWAVMA_RUNTIME_H
#define AWAVMA_RUNTIME_H

#include "runtime_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AWAVMA_RUNTIME_MONITORING,
    AWAVMA_RUNTIME_INSUFFICIENT,
    AWAVMA_RUNTIME_REJECTED,
    AWAVMA_RUNTIME_MIGRATION_METADATA_UNAVAILABLE,
    AWAVMA_RUNTIME_FEEDBACK_PENDING,
    AWAVMA_RUNTIME_TARGET_GONE,
    AWAVMA_RUNTIME_ERROR,
    AWAVMA_RUNTIME_AMBIGUOUS
} awavma_runtime_status_t;

typedef enum {
    AWAVMA_PHASE4_SUBPROCESS,
    AWAVMA_PHASE4_IN_PROCESS
} awavma_phase4_mode_t;

typedef struct {
    uint64_t monitor_interval_ms;
    uint64_t discovery_interval_ms;
    uint64_t evaluation_interval_ms;
    size_t max_applications;
    size_t worker_count;
    size_t queue_capacity;
    awavma_phase4_mode_t phase4_mode;
    const char *root_dir;
    const char *bin_dir;
    const char *phase_config_path;
    application_discovery_config_t discovery_config;
    runtime_monitor_application_filter_fn application_filter;
    void *application_filter_context;
} awavma_runtime_config_t;

typedef struct {
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    uint64_t generation;
    size_t phase3_samples;
    size_t phase5_committed_rows;
    awavma_runtime_status_t status;
    char detail[128];
} awavma_runtime_record_t;

typedef struct awavma_runtime awavma_runtime_t;

void awavma_runtime_config_default(awavma_runtime_config_t *config);
const char *awavma_runtime_status_name(awavma_runtime_status_t status);
awavma_runtime_t *awavma_runtime_create(void);
int awavma_runtime_init(awavma_runtime_t *runtime,
                        const awavma_runtime_config_t *config);
int awavma_runtime_run_for(awavma_runtime_t *runtime, uint64_t duration_ms);
void awavma_runtime_request_stop(awavma_runtime_t *runtime);
size_t awavma_runtime_snapshot(const awavma_runtime_t *runtime,
                               awavma_runtime_record_t *records,
                               size_t capacity);
void awavma_runtime_shutdown(awavma_runtime_t *runtime);
void awavma_runtime_destroy(awavma_runtime_t *runtime);

#endif
