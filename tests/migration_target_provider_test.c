#include "migration_target_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report(const char *id, int passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
}

static MigrationTargetInput input_for(ValidationAction action)
{
    MigrationTargetInput input;

    memset(&input, 0, sizeof(input));
    input.pid = 42;
    input.start_time_ticks = 99;
    input.attempt_id = "attempt-1";
    input.action = action;
    input.source_node_available = true;
    input.source_numa_node = 0;
    input.source = MIGRATION_TARGET_SOURCE_PHASE5;
    return input;
}

static MigrationTargetTopology two_node_topology(void)
{
    MigrationTargetTopology topology;

    memset(&topology, 0, sizeof(topology));
    topology.available = true;
    topology.node_present[0] = true;
    topology.node_present[1] = true;
    topology.cpu_node[0] = 0;
    topology.cpu_node[1] = 0;
    topology.cpu_node[2] = 1;
    topology.cpu_node[3] = 1;
    CPU_SET(0, &topology.online_cpus);
    CPU_SET(1, &topology.online_cpus);
    CPU_SET(2, &topology.online_cpus);
    CPU_SET(3, &topology.online_cpus);
    return topology;
}

int main(void)
{
    MigrationTargetTopology topology = two_node_topology();
    MigrationTargetTopology single = topology;
    MigrationTargetInput input;
    MigrationTarget target;
    int passed = 1;

    single.node_present[1] = false;
    input = input_for(VALIDATION_ACTION_MOVE_THREAD);
    passed = migration_target_provider_get(&input, &topology, &target) == MIGRATION_TARGET_UNAVAILABLE &&
             !target.has_target_cpu_mask && !target.has_target_numa_node;
    report("TP01_NO_TARGET", passed);
    input = input_for(VALIDATION_ACTION_MOVE_MEMORY);
    input.requires_cross_node = true;
    passed = passed && migration_target_provider_get(&input, &single, &target) ==
             MIGRATION_TARGET_NO_ALTERNATE_TARGET;
    report("TP02_SINGLE_NODE", passed);
    input = input_for(VALIDATION_ACTION_MOVE_MEMORY);
    input.has_authoritative_numa_node = true;
    input.authoritative_numa_node = 7;
    passed = passed && migration_target_provider_get(&input, &topology, &target) == MIGRATION_TARGET_INVALID;
    report("TP03_NODE_INVALID", passed);
    input = input_for(VALIDATION_ACTION_MOVE_THREAD);
    input.has_authoritative_cpu_mask = true;
    passed = passed && migration_target_provider_get(&input, &topology, &target) == MIGRATION_TARGET_INVALID;
    report("TP04_EMPTY_MASK", passed);
    CPU_SET(7, &input.authoritative_cpu_mask);
    passed = passed && migration_target_provider_get(&input, &topology, &target) == MIGRATION_TARGET_INVALID;
    report("TP05_OFFLINE_CPU", passed);
    input = input_for(VALIDATION_ACTION_MOVE_THREAD);
    input.has_authoritative_cpu_mask = true;
    CPU_SET(2, &input.authoritative_cpu_mask);
    input.has_authoritative_numa_node = true;
    input.authoritative_numa_node = 1;
    passed = passed && migration_target_provider_get(&input, &topology, &target) == MIGRATION_TARGET_AVAILABLE &&
             target.pid == input.pid && target.start_time_ticks == input.start_time_ticks &&
             strcmp(target.attempt_id, input.attempt_id) == 0 && target.has_target_cpu_mask &&
             CPU_ISSET(2, &target.target_cpu_mask) && target.has_target_numa_node &&
             target.target_numa_node == 1;
    report("TP09_SYNTHETIC_TWO_NODE", passed);
    memset(&target, 0, sizeof(target));
    passed = passed && !target.has_target_cpu_mask && !target.has_target_numa_node;
    report("TP10_ZERO_IS_NOT_TARGET_ZERO", passed);
    input = input_for(VALIDATION_ACTION_MOVE_MEMORY);
    passed = passed && migration_target_provider_get(&input, &topology, &target) == MIGRATION_TARGET_UNAVAILABLE;
    report("TP12_MEMORY_REQUIRES_NODE", passed);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
