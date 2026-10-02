#ifndef AWAVMA_THREAD_TARGET_POLICY_H
#define AWAVMA_THREAD_TARGET_POLICY_H

#include "migration_target_provider.h"

#include <stdbool.h>

/*
 * This policy selects only a destination for an already-authorized thread
 * action. It neither changes the Phase 5 action nor controls execution.
 */
typedef struct {
    pid_t pid;
    pid_t tid;
    uint64_t start_time_ticks;
    const char *attempt_id;
    ValidationAction action;
    bool migration_intent_approved;
    bool identity_match;
    bool safety_state_available;
    bool cooldown_active;
    bool quarantined;
    bool source_cpu_available;
    int source_cpu;
    bool source_node_available;
    int source_numa_node;
    bool requested_destination_available;
    int requested_destination_node;
    bool permitted_cpu_set_available;
    cpu_set_t permitted_cpu_set;
    bool history_available;
    bool recent_equivalent_failure;
    ValidationAction previous_action;
    int previous_source_node;
    int previous_target_node;
} ThreadTargetPolicyInput;

/* A preview evaluates the same policy without changing manager or process state. */
typedef struct {
    MigrationTargetResult result;
    MigrationTarget target;
} ThreadTargetPreview;

MigrationTargetResult thread_target_policy_select(const ThreadTargetPolicyInput *input,
                                                   const MigrationTargetTopology *topology,
                                                   MigrationTarget *target);
bool thread_target_policy_preview(const ThreadTargetPolicyInput *input,
                                  const MigrationTargetTopology *topology,
                                  ThreadTargetPreview *preview);

#endif
