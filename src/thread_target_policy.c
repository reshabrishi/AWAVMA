#include "thread_target_policy.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int select_destination_cpu(pid_t tid, const cpu_set_t *eligible)
{
    int cpus[CPU_SETSIZE];
    unsigned count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, eligible)) cpus[count++] = cpu;
    return cpus[(unsigned)tid % count];
}

static MigrationTargetResult reject(MigrationTarget *target, MigrationTargetResult result,
                                    const char *format, ...)
{
    if (target != NULL) {
        va_list arguments;

        target->policy_result = result;
        va_start(arguments, format);
        vsnprintf(target->reason, sizeof(target->reason), format, arguments);
        va_end(arguments);
    }
    return result;
}

static void initialize_target(const ThreadTargetPolicyInput *input, MigrationTarget *target)
{
    memset(target, 0, sizeof(*target));
    target->target_numa_node = -1;
    target->source_numa_node = -1;
    target->policy_result = MIGRATION_TARGET_INTERNAL_ERROR;
    if (input == NULL)
        return;
    target->action = input->action;
    target->pid = input->pid;
    target->start_time_ticks = input->start_time_ticks;
    if (input->attempt_id != NULL)
        snprintf(target->attempt_id, sizeof(target->attempt_id), "%s", input->attempt_id);
    target->source = MIGRATION_TARGET_SOURCE_RUNTIME_POLICY;
}

static MigrationTargetResult source_from_affinity(const ThreadTargetPolicyInput *input,
                                                  const MigrationTargetTopology *topology,
                                                  int *source_node)
{
    int observed_node = -1;

    if (!input->permitted_cpu_set_available)
        return MIGRATION_TARGET_SOURCE_UNKNOWN;
    if (input->source_cpu_available) {
        if (input->source_cpu < 0 || input->source_cpu >= CPU_SETSIZE ||
            !CPU_ISSET(input->source_cpu, &topology->online_cpus) ||
             !CPU_ISSET(input->source_cpu, &input->permitted_cpu_set) ||
            topology->cpu_node[input->source_cpu] < 0)
            return MIGRATION_TARGET_SOURCE_AMBIGUOUS;
        *source_node = topology->cpu_node[input->source_cpu];
        return MIGRATION_TARGET_AVAILABLE;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        int node;

        if (!CPU_ISSET(cpu, &input->permitted_cpu_set))
            continue;
        if (!CPU_ISSET(cpu, &topology->online_cpus) || topology->cpu_node[cpu] < 0)
            return MIGRATION_TARGET_SOURCE_UNKNOWN;
        node = topology->cpu_node[cpu];
        if (observed_node >= 0 && observed_node != node)
            return MIGRATION_TARGET_SOURCE_AMBIGUOUS;
        observed_node = node;
    }
    if (observed_node < 0)
        return MIGRATION_TARGET_SOURCE_UNKNOWN;
    *source_node = observed_node;
    return MIGRATION_TARGET_AVAILABLE;
}

MigrationTargetResult thread_target_policy_select(const ThreadTargetPolicyInput *input,
                                                  const MigrationTargetTopology *topology,
                                                  MigrationTarget *target)
{
    MigrationTargetTopology live;
    const MigrationTargetTopology *active = topology;
    MigrationTargetInput provider_input;
    MigrationTarget provider_target;
    cpu_set_t candidate_mask;
    int source_node = -1;
    int candidate_node = -1;
    unsigned alternative_nodes = 0;
    unsigned candidate_nodes = 0;
    unsigned suppressed_nodes = 0;
    MigrationTargetResult result;

    if (target == NULL)
        return MIGRATION_TARGET_INTERNAL_ERROR;
    initialize_target(input, target);
    if (input == NULL || input->pid <= 0 || input->start_time_ticks == 0 ||
        input->attempt_id == NULL || input->attempt_id[0] == '\0')
        return reject(target, MIGRATION_TARGET_SOURCE_UNKNOWN,
                      "result=source_unknown reason=identity_metadata_unavailable");
    if (!input->migration_intent_approved)
        return reject(target, MIGRATION_TARGET_NO_MIGRATION_INTENT,
                      "result=no_migration_intent reason=upstream_intent_not_approved");
    if (input->action != VALIDATION_ACTION_MOVE_THREAD && input->action != VALIDATION_ACTION_MOVE_MEMORY)
        return reject(target, MIGRATION_TARGET_UNSUPPORTED_ACTION,
                      "result=unsupported_action reason=thread_policy_only");
    if (!input->identity_match)
        return reject(target, MIGRATION_TARGET_IDENTITY_CHANGED,
                      "result=identity_changed reason=runtime_identity_mismatch");
    if (!input->safety_state_available)
        return reject(target, MIGRATION_TARGET_INTERNAL_ERROR,
                      "result=error reason=safety_state_unavailable");
    if (input->quarantined)
        return reject(target, MIGRATION_TARGET_QUARANTINED,
                      "result=quarantined reason=application_quarantined");
    if (input->cooldown_active)
        return reject(target, MIGRATION_TARGET_COOLDOWN,
                      "result=cooldown reason=application_cooldown_active");
    if (active == NULL) {
        if (!migration_target_topology_read(&live))
            return reject(target, MIGRATION_TARGET_TOPOLOGY_UNAVAILABLE,
                          "result=topology_unavailable reason=online_topology_unreadable");
        active = &live;
    }
    if (!active->available)
        return reject(target, MIGRATION_TARGET_TOPOLOGY_UNAVAILABLE,
                      "result=topology_unavailable reason=no_online_numa_cpus");
    if (input->source_node_available) {
        source_node = input->source_numa_node;
        result = MIGRATION_TARGET_AVAILABLE;
    } else
        result = source_from_affinity(input, active, &source_node);
    if (result != MIGRATION_TARGET_AVAILABLE)
        return reject(target, result, result == MIGRATION_TARGET_SOURCE_AMBIGUOUS ?
                      "result=source_ambiguous reason=affinity_spans_nodes_without_current_cpu" :
                      "result=source_unknown reason=no_authoritative_source_placement");
    if (source_node < 0 || source_node >= (int)MIGRATION_TARGET_MAX_NODES ||
        !active->node_present[source_node])
        return reject(target, MIGRATION_TARGET_SOURCE_UNKNOWN,
                      "result=source_unknown reason=source_cpu_has_no_online_numa_node");
    target->source_node_known = true;
    target->source_numa_node = source_node;
    if (!input->requested_destination_available)
        return reject(target, MIGRATION_TARGET_INVALID,
                      "result=invalid reason=requested_destination_unavailable");
    if (input->requested_destination_node == source_node)
        return reject(target, MIGRATION_TARGET_NO_ALTERNATE_TARGET,
                      "result=no_alternate source_node=%d reason=already_on_requested_destination", source_node);
    CPU_ZERO(&candidate_mask);
    for (unsigned node = 0; node < MIGRATION_TARGET_MAX_NODES; node++) {
        cpu_set_t node_mask;
        bool node_has_online_cpu = false;
        bool suppressed = input->history_available && input->recent_equivalent_failure &&
                           input->previous_action == input->action &&
                           input->previous_source_node == source_node &&
                           input->previous_target_node == (int)node;

        if ((int)node != input->requested_destination_node)
            continue;
        if (!active->node_present[node] || (int)node == source_node)
            continue;
        CPU_ZERO(&node_mask);
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (active->cpu_node[cpu] != (int)node || !CPU_ISSET(cpu, &active->online_cpus))
                continue;
            node_has_online_cpu = true;
            if (CPU_ISSET(cpu, &input->permitted_cpu_set))
                CPU_SET(cpu, &node_mask);
        }
        /* A sysfs-present node with no online CPU is not a destination candidate. */
        if (!node_has_online_cpu)
            continue;
        alternative_nodes++;
        if (CPU_COUNT(&node_mask) == 0)
            continue;
        if (suppressed) {
            suppressed_nodes++;
            continue;
        }
        candidate_nodes++;
        candidate_node = (int)node;
        candidate_mask = node_mask;
    }
    target->candidate_count = candidate_nodes;
    if (candidate_nodes == 0) {
        if (suppressed_nodes > 0)
            return reject(target, MIGRATION_TARGET_SUPPRESSED_BY_HISTORY,
                          "result=suppressed_by_history candidates=0 reason=recent_equivalent_target_failure");
        if (alternative_nodes == 0)
            return reject(target, MIGRATION_TARGET_NO_ALTERNATE_TARGET,
                          "result=no_alternate source_node=%d candidates=0 reason=no_other_online_numa_node",
                          source_node);
        return reject(target, MIGRATION_TARGET_NO_ELIGIBLE_CPUS,
                      "result=no_eligible_cpus source_node=%d candidates=0 reason=permitted_cpu_set_has_no_requested_node_cpu",
                      source_node);
    }
    int selected_cpu = select_destination_cpu(input->tid, &candidate_mask);
    CPU_ZERO(&candidate_mask);
    CPU_SET(selected_cpu, &candidate_mask);
    memset(&provider_input, 0, sizeof(provider_input));
    provider_input.pid = input->pid;
    provider_input.start_time_ticks = input->start_time_ticks;
    provider_input.attempt_id = input->attempt_id;
    provider_input.action = input->action;
    provider_input.source_node_available = true;
    provider_input.source_numa_node = source_node;
    provider_input.requires_cross_node = true;
    provider_input.has_authoritative_cpu_mask = true;
    provider_input.authoritative_cpu_mask = candidate_mask;
    provider_input.has_authoritative_numa_node = true;
    provider_input.authoritative_numa_node = candidate_node;
    provider_input.source = MIGRATION_TARGET_SOURCE_RUNTIME_POLICY;
    result = migration_target_provider_get(&provider_input, active, &provider_target);
    if (result != MIGRATION_TARGET_AVAILABLE) {
        *target = provider_target;
        target->source_node_known = true;
        target->source_numa_node = source_node;
        target->candidate_count = candidate_nodes;
        target->policy_result = result;
        snprintf(target->reason, sizeof(target->reason),
                 "result=%s source_node=%d candidates=%u reason=provider_rejected_candidate",
                 migration_target_result_name(result), source_node, candidate_nodes);
        return result;
    }
    *target = provider_target;
    target->source_node_known = true;
    target->source_numa_node = source_node;
    target->candidate_count = candidate_nodes;
    target->policy_result = MIGRATION_TARGET_AVAILABLE;
    snprintf(target->reason, sizeof(target->reason),
              "result=selected action=%s source_node=%d candidates=1 selected_node=%d reason=single_valid_alternative",
              input->action == VALIDATION_ACTION_MOVE_MEMORY ? "MOVE_MEMORY" : "MOVE_THREAD",
              source_node, candidate_node);
    return MIGRATION_TARGET_AVAILABLE;
}

bool thread_target_policy_preview(const ThreadTargetPolicyInput *input,
                                  const MigrationTargetTopology *topology,
                                  ThreadTargetPreview *preview)
{
    if (preview == NULL)
        return false;
    memset(preview, 0, sizeof(*preview));
    preview->target.source_numa_node = -1;
    preview->target.target_numa_node = -1;
    if (input == NULL || input->action != VALIDATION_ACTION_MOVE_THREAD) {
        preview->result = MIGRATION_TARGET_UNSUPPORTED_ACTION;
        snprintf(preview->target.reason, sizeof(preview->target.reason),
                 "result=unsupported_action reason=thread_preview_only");
        return false;
    }
    preview->result = thread_target_policy_select(input, topology, &preview->target);
    return preview->result == MIGRATION_TARGET_AVAILABLE;
}
