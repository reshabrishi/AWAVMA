#ifndef AWAVMA_MIGRATION_TARGET_PROVIDER_H
#define AWAVMA_MIGRATION_TARGET_PROVIDER_H

#include "migration_types.h"

#include <stdbool.h>

#define MIGRATION_TARGET_MAX_NODES 64U

typedef enum {
    MIGRATION_TARGET_AVAILABLE,
    MIGRATION_TARGET_NO_MIGRATION_INTENT,
    MIGRATION_TARGET_NO_ALTERNATE_TARGET,
    MIGRATION_TARGET_SOURCE_UNKNOWN,
    MIGRATION_TARGET_SOURCE_AMBIGUOUS,
    MIGRATION_TARGET_NO_ELIGIBLE_CPUS,
    MIGRATION_TARGET_AMBIGUOUS,
    MIGRATION_TARGET_SUPPRESSED_BY_HISTORY,
    MIGRATION_TARGET_COOLDOWN,
    MIGRATION_TARGET_QUARANTINED,
    MIGRATION_TARGET_PAGE_RECOVERY_UNAVAILABLE,
    MIGRATION_TARGET_UNSUPPORTED_ACTION,
    MIGRATION_TARGET_IDENTITY_CHANGED,
    MIGRATION_TARGET_TOPOLOGY_UNAVAILABLE,
    MIGRATION_TARGET_INVALID,
    MIGRATION_TARGET_STALE,
    MIGRATION_TARGET_INTERNAL_ERROR,
    /* Compatibility aliases for generic provider callers. */
    MIGRATION_TARGET_UNAVAILABLE = MIGRATION_TARGET_NO_MIGRATION_INTENT,
    MIGRATION_TARGET_INVALID_SOURCE_METADATA = MIGRATION_TARGET_SOURCE_UNKNOWN,
    MIGRATION_TARGET_POLICY_REJECTED = MIGRATION_TARGET_AMBIGUOUS
} MigrationTargetResult;

typedef enum {
    MIGRATION_TARGET_SOURCE_NONE,
    MIGRATION_TARGET_SOURCE_PHASE5,
    MIGRATION_TARGET_SOURCE_RUNTIME_POLICY,
    MIGRATION_TARGET_SOURCE_TEST
} MigrationTargetSource;

typedef struct {
    bool available;
    bool node_present[MIGRATION_TARGET_MAX_NODES];
    cpu_set_t online_cpus;
    int cpu_node[CPU_SETSIZE];
} MigrationTargetTopology;

typedef struct {
    pid_t pid;
    uint64_t start_time_ticks;
    const char *attempt_id;
    ValidationAction action;
    bool source_node_available;
    int source_numa_node;
    bool requires_cross_node;
    bool has_authoritative_cpu_mask;
    cpu_set_t authoritative_cpu_mask;
    bool has_authoritative_numa_node;
    int authoritative_numa_node;
    MigrationTargetSource source;
} MigrationTargetInput;

typedef struct {
    ValidationAction action;
    pid_t pid;
    uint64_t start_time_ticks;
    char attempt_id[128];
    bool has_target_cpu_mask;
    cpu_set_t target_cpu_mask;
    bool permitted_cpu_set_available;
    cpu_set_t permitted_cpu_set;
    bool thread_start_time_ticks_available;
    uint64_t thread_start_time_ticks;
    bool has_target_numa_node;
    int target_numa_node;
    bool source_node_known;
    int source_numa_node;
    unsigned candidate_count;
    MigrationTargetResult policy_result;
    MigrationTargetSource source;
    char reason[256];
} MigrationTarget;

const char *migration_target_result_name(MigrationTargetResult result);
bool migration_target_topology_read(MigrationTargetTopology *topology);
MigrationTargetResult migration_target_provider_get(const MigrationTargetInput *input,
                                                    const MigrationTargetTopology *topology,
                                                    MigrationTarget *target);

#endif
