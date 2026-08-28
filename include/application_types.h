#ifndef AWAVMA_APPLICATION_TYPES_H
#define AWAVMA_APPLICATION_TYPES_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef enum {
    APPLICATION_DISCOVERED,
    APPLICATION_ACTIVE,
    APPLICATION_INACTIVE,
    APPLICATION_TERMINATED,
    APPLICATION_EXPIRED
} application_status_t;

typedef struct {
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    char process_name[256];
    char executable[PATH_MAX];
    time_t discovered_at;
    time_t last_seen;
    application_status_t status;
    int worker_id;
    unsigned monitor_interval_ms;
    bool job_in_progress;
} application_identity_t;

const char *application_status_name(application_status_t status);

#endif
