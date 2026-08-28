#ifndef AWAVMA_APPLICATION_MANAGER_TYPES_H
#define AWAVMA_APPLICATION_MANAGER_TYPES_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef enum {
    APPLICATION_MANAGER_DISCOVERED,
    APPLICATION_MANAGER_ACTIVE,
    APPLICATION_MANAGER_TERMINATED,
    APPLICATION_MANAGER_EXPIRED
} application_manager_status_t;

typedef struct {
    char app_id[128];
    pid_t pid;
    pid_t parent_pid;
    char process_name[256];
    char executable_path[PATH_MAX];
    uint64_t start_time_ticks;
    char state;
    time_t discovered_at;
    time_t last_seen;
    application_manager_status_t status;
} application_manager_record_t;

#endif
