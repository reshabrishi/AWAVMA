#ifndef AWAVMA_ENVIRONMENT_CAPABILITIES_H
#define AWAVMA_ENVIRONMENT_CAPABILITIES_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ENVIRONMENT_READY,
    ENVIRONMENT_ENV_LIMITED,
    ENVIRONMENT_ERROR
} EnvironmentCapabilityState;

typedef struct {
    EnvironmentCapabilityState state;
    bool linux_supported;
    bool numa_topology_available;
    size_t online_numa_nodes;
    bool cpu_node_topology_valid;
    bool cross_node_destination_available;
    bool sched_getaffinity_ready;
    bool sched_setaffinity_ready;
    bool move_pages_available;
    bool move_pages_ready;
    bool thread_migration_ready;
    bool page_migration_ready;
    bool phase7_ready;
    /* Strict capability conjunction required by the production real-migration profile. */
    bool production_real_migration_ready;
    char reason[160];
} EnvironmentCapabilities;

const char *environment_capability_state_name(EnvironmentCapabilityState state);
bool environment_capabilities_production_real_migration_ready(const EnvironmentCapabilities *capabilities);
bool environment_capabilities_detect(EnvironmentCapabilities *capabilities);

#endif
