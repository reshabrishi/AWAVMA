#ifndef AWAVMA_MIGRATION_TYPES_H
#define AWAVMA_MIGRATION_TYPES_H

#include "validation.h"

#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    MIGRATION_SUCCESS,
    MIGRATION_NO_ACTION,
    MIGRATION_NO_MIGRATION_REQUIRED,
    MIGRATION_NOT_AUTHORIZED,
    MIGRATION_INSUFFICIENT_INFORMATION,
    MIGRATION_TARGET_GONE,
    MIGRATION_INVALID_SOURCE,
    MIGRATION_INVALID_DESTINATION,
    MIGRATION_INVALID_TARGET,
    MIGRATION_PERMISSION_DENIED,
    MIGRATION_UNSUPPORTED,
    MIGRATION_SYSTEM_ERROR,
    MIGRATION_PARTIAL_SUCCESS,
    MIGRATION_VERIFICATION_UNAVAILABLE,
    MIGRATION_ALREADY_IN_PROGRESS
} MigrationResultCode;

typedef struct {
    DecisionData phase5_decision;
    ValidationResult phase6_validation;
    pid_t pid;
    uint64_t start_time_ticks;
    bool start_time_ticks_available;
    pid_t tid;
    int source_numa_node;
    int destination_numa_node;
    bool numa_nodes_available;
    int destination_cpu;
    cpu_set_t requested_cpu_set;
    bool requested_cpu_set_available;
    void **pages;
    size_t page_count;
    bool page_metadata_available;
    bool memory_region_verified;
    bool explicit_placement_required;
    bool memory_locked;
    bool memory_pinned;
} MigrationRequest;

typedef struct {
    char timestamp[32];
    char migration_id[128];
    char app_id[128];
    char entity_id[128];
    pid_t pid;
    uint64_t expected_start_time_ticks;
    pid_t tid;
    ValidationAction action;
    char phase5_decision[40];
    char phase6_validation[64];
    int source_numa_node;
    int destination_numa_node;
    int destination_cpu;
    size_t pages_requested;
    size_t pages_attempted;
    size_t pages_migrated;
    size_t pages_failed;
    double execution_time_ms;
    char verification_status[64];
    char old_affinity[256];
    char requested_affinity[256];
    char new_affinity[256];
    char error_reason[128];
    int errno_value;
    MigrationResultCode result;
} MigrationReport;

#endif
