#ifndef AWAVMA_RUNTIME_MIGRATION_METADATA_H
#define AWAVMA_RUNTIME_MIGRATION_METADATA_H

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    uint64_t captured_at_ms;
    pid_t pid;
    uint64_t start_time_ticks;
    bool identity_match;
    bool process_exists;
    bool affinity_available;
    cpu_set_t affinity;
    bool current_cpu_available;
    int current_cpu;
    bool current_numa_node_available;
    int current_numa_node;
    bool process_cpu_time_available;
    uint64_t process_cpu_time_ticks;
    bool page_placement_available;
} RuntimeMigrationMetadata;

typedef struct {
    uint64_t captured_at_ms;
    pid_t pid;
    uint64_t start_time_ticks;
    bool affinity_available;
    cpu_set_t original_affinity;
    bool page_rollback_checkpoint_available;
} RuntimeMigrationCheckpoint;

typedef enum {
    RUNTIME_MIGRATION_ROLLBACK_COMPLETE,
    RUNTIME_MIGRATION_ROLLBACK_TARGET_GONE,
    RUNTIME_MIGRATION_ROLLBACK_IDENTITY_CHANGED,
    RUNTIME_MIGRATION_ROLLBACK_CHECKPOINT_UNAVAILABLE,
    RUNTIME_MIGRATION_ROLLBACK_FAILED,
    RUNTIME_MIGRATION_ROLLBACK_VERIFICATION_FAILED
} RuntimeMigrationRollbackResult;

/* Reads only live Linux state. Unavailable observations remain unavailable. */
bool runtime_get_migration_metadata(pid_t pid, uint64_t start_time_ticks,
                                    RuntimeMigrationMetadata *metadata);
bool runtime_migration_checkpoint_affinity(const RuntimeMigrationMetadata *metadata,
                                           RuntimeMigrationCheckpoint *checkpoint);
RuntimeMigrationRollbackResult runtime_migration_restore_affinity(
    const RuntimeMigrationCheckpoint *checkpoint);
void runtime_migration_checkpoint_release(RuntimeMigrationCheckpoint *checkpoint);

#endif
