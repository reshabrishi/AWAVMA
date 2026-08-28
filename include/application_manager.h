#ifndef AWAVMA_APPLICATION_MANAGER_H
#define AWAVMA_APPLICATION_MANAGER_H

#include "application_discovery.h"
#include "application_manager_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t max_applications;
    uint64_t application_idle_timeout_ms;
    const char *log_path;
    const char *results_path;
    const char *table_path;
} application_manager_config_t;

typedef struct application_manager application_manager_t;

void application_manager_config_default(application_manager_config_t *config);
application_manager_t *application_manager_create(void);
int application_manager_init(application_manager_t *manager,
                             const application_manager_config_t *config);
int application_manager_process_snapshot(application_manager_t *manager,
                                         const application_discovery_record_t *records,
                                         size_t count);
bool application_manager_lookup_app_id(const application_manager_t *manager,
                                       const char *app_id,
                                       application_manager_record_t *record);
bool application_manager_lookup_identity(const application_manager_t *manager,
                                         pid_t pid, uint64_t start_time_ticks,
                                         application_manager_record_t *record);
size_t application_manager_active_count(const application_manager_t *manager);
size_t application_manager_snapshot(const application_manager_t *manager,
                                   application_manager_record_t *records,
                                   size_t capacity);
const char *application_manager_status_name(application_manager_status_t status);
void application_manager_shutdown(application_manager_t *manager);
void application_manager_destroy(application_manager_t *manager);

#endif
