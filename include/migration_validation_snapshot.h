#ifndef AWAVMA_MIGRATION_VALIDATION_SNAPSHOT_H
#define AWAVMA_MIGRATION_VALIDATION_SNAPSHOT_H

#include "runtime_migration_metadata.h"

typedef enum {
    MIGRATION_VALIDATION_STRUCTURALLY_VALID,
    MIGRATION_VALIDATION_PLACEMENT_NOT_APPLIED,
    MIGRATION_VALIDATION_INSUFFICIENT_DATA,
    MIGRATION_VALIDATION_TARGET_GONE,
    MIGRATION_VALIDATION_IDENTITY_CHANGED,
    MIGRATION_VALIDATION_STRUCTURALLY_VALID_PROGRESS,
    MIGRATION_VALIDATION_NO_PROGRESS_INCONCLUSIVE
} MigrationValidationOutcome;

typedef struct {
    pid_t pid;
    pid_t tid;
    uint64_t start_time_ticks;
    uint64_t thread_start_time_ticks;
    char attempt_id[128];
    uint64_t monotonic_ms;
    bool process_exists;
    bool identity_match;
    bool affinity_available;
    cpu_set_t affinity;
    bool process_cpu_time_available;
    uint64_t process_cpu_time_ticks;
} MigrationValidationSnapshot;

bool migration_validation_snapshot_collect(pid_t pid, pid_t tid, uint64_t start_time_ticks,
                                            uint64_t thread_start_time_ticks,
                                            const char *attempt_id,
                                           MigrationValidationSnapshot *snapshot);
MigrationValidationOutcome migration_validation_compare(
    const MigrationValidationSnapshot *before, const MigrationValidationSnapshot *after,
    const cpu_set_t *requested_affinity, bool requested_affinity_available);

#endif
