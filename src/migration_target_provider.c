#define _POSIX_C_SOURCE 200809L

#include "migration_target_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_cpu_list(const char *text, cpu_set_t *set)
{
    const char *cursor = text;

    CPU_ZERO(set);
    while (*cursor != '\0' && *cursor != '\n') {
        char *end;
        long first;
        long last;

        first = strtol(cursor, &end, 10);
        if (end == cursor || first < 0 || first >= CPU_SETSIZE)
            return false;
        last = first;
        if (*end == '-') {
            cursor = end + 1;
            last = strtol(cursor, &end, 10);
            if (end == cursor || last < first || last >= CPU_SETSIZE)
                return false;
        }
        for (long cpu = first; cpu <= last; cpu++)
            CPU_SET((int)cpu, set);
        if (*end == ',')
            cursor = end + 1;
        else if (*end == '\0' || *end == '\n')
            break;
        else
            return false;
    }
    return CPU_COUNT(set) > 0;
}

bool migration_target_topology_read(MigrationTargetTopology *topology)
{
    char path[128];
    char cpulist[4096];
    cpu_set_t online;
    FILE *file;

    if (topology == NULL)
        return false;
    memset(topology, 0, sizeof(*topology));
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        topology->cpu_node[cpu] = -1;
    file = fopen("/sys/devices/system/cpu/online", "r");
    if (file == NULL || fgets(cpulist, sizeof(cpulist), file) == NULL ||
        !parse_cpu_list(cpulist, &online)) {
        if (file != NULL)
            fclose(file);
        return false;
    }
    fclose(file);
    for (unsigned node = 0; node < MIGRATION_TARGET_MAX_NODES; node++) {
        cpu_set_t node_cpus;

        if (snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/cpulist", node) >= (int)sizeof(path))
            return false;
        file = fopen(path, "r");
        if (file == NULL)
            continue;
        if (fgets(cpulist, sizeof(cpulist), file) == NULL || !parse_cpu_list(cpulist, &node_cpus)) {
            fclose(file);
            return false;
        }
        fclose(file);
        topology->node_present[node] = true;
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
            if (CPU_ISSET(cpu, &node_cpus)) {
                if (CPU_ISSET(cpu, &online))
                    CPU_SET(cpu, &topology->online_cpus);
                topology->cpu_node[cpu] = (int)node;
            }
    }
    topology->available = CPU_COUNT(&topology->online_cpus) > 0;
    return topology->available;
}

static bool node_exists(const MigrationTargetTopology *topology, int node)
{
    return node >= 0 && node < (int)MIGRATION_TARGET_MAX_NODES && topology->node_present[node];
}

static bool mask_is_online(const cpu_set_t *mask, const MigrationTargetTopology *topology)
{
    if (CPU_COUNT(mask) == 0)
        return false;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, mask) && !CPU_ISSET(cpu, &topology->online_cpus))
            return false;
    return true;
}

const char *migration_target_result_name(MigrationTargetResult result)
{
    static const char *names[] = {"TARGET_POLICY_SELECTED", "TARGET_POLICY_NO_MIGRATION_INTENT",
        "TARGET_POLICY_NO_ALTERNATE", "TARGET_POLICY_SOURCE_UNKNOWN",
        "TARGET_POLICY_SOURCE_AMBIGUOUS", "TARGET_POLICY_NO_ELIGIBLE_CPUS",
        "TARGET_POLICY_AMBIGUOUS", "TARGET_POLICY_SUPPRESSED_BY_HISTORY",
        "TARGET_POLICY_COOLDOWN", "TARGET_POLICY_QUARANTINED",
        "PAGE_RECOVERY_UNAVAILABLE", "TARGET_POLICY_UNSUPPORTED_ACTION",
        "TARGET_POLICY_IDENTITY_CHANGED", "TARGET_POLICY_TOPOLOGY_UNAVAILABLE",
        "TARGET_POLICY_INVALID", "TARGET_POLICY_STALE", "TARGET_POLICY_ERROR"};
    return result >= MIGRATION_TARGET_AVAILABLE && result <= MIGRATION_TARGET_INTERNAL_ERROR ?
        names[result] : "UNKNOWN";
}

MigrationTargetResult migration_target_provider_get(const MigrationTargetInput *input,
                                                    const MigrationTargetTopology *topology,
                                                    MigrationTarget *target)
{
    const MigrationTargetTopology *active = topology;
    MigrationTargetTopology live;

    if (target == NULL)
        return MIGRATION_TARGET_INTERNAL_ERROR;
    memset(target, 0, sizeof(*target));
    target->target_numa_node = -1;
    target->source_numa_node = -1;
    if (input == NULL || input->pid <= 0 || input->start_time_ticks == 0 ||
        input->attempt_id == NULL || input->attempt_id[0] == '\0')
        return MIGRATION_TARGET_SOURCE_UNKNOWN;
    target->action = input->action;
    target->pid = input->pid;
    target->start_time_ticks = input->start_time_ticks;
    target->source = input->source;
    snprintf(target->attempt_id, sizeof(target->attempt_id), "%s", input->attempt_id);
    if (input->source_node_available) {
        target->source_node_known = true;
        target->source_numa_node = input->source_numa_node;
    }
    if (input->action != VALIDATION_ACTION_MOVE_THREAD && input->action != VALIDATION_ACTION_MOVE_MEMORY)
        return MIGRATION_TARGET_UNSUPPORTED_ACTION;
    if (active == NULL) {
        if (!migration_target_topology_read(&live))
            return MIGRATION_TARGET_TOPOLOGY_UNAVAILABLE;
        active = &live;
    }
    if (!active->available)
        return MIGRATION_TARGET_TOPOLOGY_UNAVAILABLE;
    if (input->requires_cross_node) {
        unsigned count = 0;
        for (unsigned node = 0; node < MIGRATION_TARGET_MAX_NODES; node++)
            if (active->node_present[node])
                count++;
        if (count < 2)
            return MIGRATION_TARGET_NO_ALTERNATE_TARGET;
    }
    if (input->action == VALIDATION_ACTION_MOVE_MEMORY) {
        if (!input->has_authoritative_numa_node)
            return MIGRATION_TARGET_UNAVAILABLE;
        if (!node_exists(active, input->authoritative_numa_node) ||
            (input->requires_cross_node && input->source_node_available &&
             input->authoritative_numa_node == input->source_numa_node))
            return MIGRATION_TARGET_INVALID;
        target->has_target_numa_node = true;
        target->target_numa_node = input->authoritative_numa_node;
    } else {
        if (!input->has_authoritative_cpu_mask)
            return MIGRATION_TARGET_UNAVAILABLE;
        if (!mask_is_online(&input->authoritative_cpu_mask, active))
            return MIGRATION_TARGET_INVALID;
        target->has_target_cpu_mask = true;
        target->target_cpu_mask = input->authoritative_cpu_mask;
        if (input->has_authoritative_numa_node) {
            if (!node_exists(active, input->authoritative_numa_node))
                return MIGRATION_TARGET_INVALID;
            for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
                if (CPU_ISSET(cpu, &input->authoritative_cpu_mask) &&
                    active->cpu_node[cpu] != input->authoritative_numa_node)
                    return MIGRATION_TARGET_INVALID;
            target->has_target_numa_node = true;
            target->target_numa_node = input->authoritative_numa_node;
        }
    }
    snprintf(target->reason, sizeof(target->reason), "authoritative target accepted");
    target->policy_result = MIGRATION_TARGET_AVAILABLE;
    return MIGRATION_TARGET_AVAILABLE;
}

MigrationTargetStructure migration_target_structure_validate(const MigrationTarget *target,
                                                              bool requires_cross_node)
{
    if (target == NULL || target->pid <= 0 || target->start_time_ticks == 0 ||
        target->attempt_id[0] == '\0')
        return MIGRATION_TARGET_STRUCTURE_BINDING_INVALID;
    if (target->action == VALIDATION_ACTION_MOVE_THREAD) {
        if (!target->has_target_cpu_mask || CPU_COUNT(&target->target_cpu_mask) == 0 ||
            !target->has_target_numa_node)
            return MIGRATION_TARGET_STRUCTURE_THREAD_CPU_INVALID;
    } else if (target->action == VALIDATION_ACTION_MOVE_MEMORY) {
        if (!target->source_node_known || !target->has_target_numa_node ||
            target->source_numa_node < 0 || target->target_numa_node < 0 ||
            (requires_cross_node && target->source_numa_node == target->target_numa_node))
            return MIGRATION_TARGET_STRUCTURE_MEMORY_NUMA_INVALID;
    } else {
        return MIGRATION_TARGET_STRUCTURE_BINDING_INVALID;
    }
    return MIGRATION_TARGET_STRUCTURE_VALID;
}
