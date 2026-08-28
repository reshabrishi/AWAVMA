#ifndef AWAVMA_APPLICATION_RUNTIME_H
#define AWAVMA_APPLICATION_RUNTIME_H

#include "application_manager.h"
#include "worker_pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APPLICATION_RUNTIME_JOB_PENDING,
    APPLICATION_RUNTIME_JOB_COMPLETED,
    APPLICATION_RUNTIME_JOB_SKIPPED,
    APPLICATION_RUNTIME_JOB_REJECTED
} application_runtime_job_status_t;

typedef struct {
    size_t max_jobs;
    unsigned job_duration_ms;
    const char *log_path;
    const char *results_path;
} application_runtime_config_t;

typedef struct {
    uint64_t job_id;
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    int worker_id;
    application_runtime_job_status_t status;
    bool application_valid;
} application_runtime_job_record_t;

typedef struct application_runtime application_runtime_t;

void application_runtime_config_default(application_runtime_config_t *config);
const char *application_runtime_job_status_name(application_runtime_job_status_t status);
application_runtime_t *application_runtime_create(void);
int application_runtime_init(application_runtime_t *runtime,
                             application_manager_t *manager,
                             worker_pool_t *pool,
                             const application_runtime_config_t *config);
int application_runtime_submit_active(application_runtime_t *runtime,
                                      size_t *submitted, size_t *rejected);
int application_runtime_submit_snapshot(application_runtime_t *runtime,
                                         const application_manager_record_t *records,
                                         size_t count, size_t *submitted,
                                         size_t *rejected);
int application_runtime_wait_idle(application_runtime_t *runtime);
size_t application_runtime_snapshot(const application_runtime_t *runtime,
                                    application_runtime_job_record_t *records,
                                    size_t capacity);
size_t application_runtime_completed_count(const application_runtime_t *runtime);
void application_runtime_shutdown(application_runtime_t *runtime);
void application_runtime_destroy(application_runtime_t *runtime);

#endif
