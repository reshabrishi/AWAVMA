#ifndef AWAVMA_APPLICATION_DISCOVERY_H
#define AWAVMA_APPLICATION_DISCOVERY_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    pid_t pid;
    pid_t parent_pid;
    uint64_t start_time_ticks;
    char process_name[256];
    char executable_path[PATH_MAX];
    char state;
} application_discovery_record_t;

typedef struct {
    const char *proc_root;
    bool include_pid1;
    pid_t excluded_pid;
} application_discovery_config_t;

typedef struct application_discovery application_discovery_t;

void application_discovery_config_default(application_discovery_config_t *config);
application_discovery_t *application_discovery_create(void);
int application_discovery_init(application_discovery_t *discovery,
                               const application_discovery_config_t *config);
int application_discovery_scan(application_discovery_t *discovery);
size_t application_discovery_count(const application_discovery_t *discovery);
bool application_discovery_get(const application_discovery_t *discovery,
                               size_t index, application_discovery_record_t *record);
void application_discovery_cleanup(application_discovery_t *discovery);
void application_discovery_destroy(application_discovery_t *discovery);

#endif
