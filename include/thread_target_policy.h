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
    bool allowed_affinity_available;
    cpu_set_t allowed_affinity;
    bool history_available;
    bool recent_equivalent_failure;
    ValidationAction previous_action;
    int previous_source_node;
    int previous_target_node;
} ThreadTargetPolicyInput;

MigrationTargetResult thread_target_policy_select(const ThreadTargetPolicyInput *input,
                                                  const MigrationTargetTopology *topology,
                                                  MigrationTarget *target);

#endif
