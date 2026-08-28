#ifndef AWAVMA_RUNTIME_TARGET_FILTER_H
#define AWAVMA_RUNTIME_TARGET_FILTER_H

#include "application_manager_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    pid_t pid;
    uint64_t start_time_ticks;
} runtime_target_identity_t;

typedef struct {
    runtime_target_identity_t *identities;
    size_t count;
    size_t capacity;
} runtime_target_filter_t;

void runtime_target_filter_init(runtime_target_filter_t *filter);
void runtime_target_filter_cleanup(runtime_target_filter_t *filter);
int runtime_target_filter_add_identity(runtime_target_filter_t *filter, pid_t pid,
                                       uint64_t start_time_ticks);
int runtime_target_filter_add_pid(runtime_target_filter_t *filter, pid_t pid);
bool runtime_target_filter_matches(const application_manager_record_t *application,
                                   void *context);

#endif
