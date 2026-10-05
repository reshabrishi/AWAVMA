#define _GNU_SOURCE
#include "environment_capabilities.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

const char *environment_capability_state_name(EnvironmentCapabilityState state)
{
    switch (state) {
    case ENVIRONMENT_READY: return "READY";
    case ENVIRONMENT_ENV_LIMITED: return "ENV_LIMITED";
    default: return "ERROR";
    }
}

bool environment_capabilities_production_real_migration_ready(const EnvironmentCapabilities *capabilities)
{
    return capabilities != NULL && capabilities->online_numa_nodes >= 2 &&
           capabilities->thread_migration_ready && capabilities->page_migration_ready &&
           capabilities->phase7_ready;
}

bool environment_capabilities_detect(EnvironmentCapabilities *capabilities)
{
    cpu_set_t affinity;
    size_t nodes = 0;

    if (capabilities == NULL)
        return false;
    memset(capabilities, 0, sizeof(*capabilities));
#ifdef __linux__
    capabilities->linux_supported = true;
#else
    capabilities->state = ENVIRONMENT_ENV_LIMITED;
    snprintf(capabilities->reason, sizeof(capabilities->reason), "Linux is required");
    return true;
#endif
    for (unsigned node = 0; node < 4096; node++) {
        char path[128];
        FILE *file;

        if (snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/cpulist", node) >= (int)sizeof(path))
            break;
        file = fopen(path, "r");
        if (file != NULL) {
            char cpulist[256];
            if (fgets(cpulist, sizeof(cpulist), file) != NULL && cpulist[0] != '\n')
                nodes++;
            fclose(file);
        }
    }
    capabilities->online_numa_nodes = nodes;
    capabilities->numa_topology_available = nodes > 0;
    capabilities->cpu_node_topology_valid = nodes > 0;
    capabilities->cross_node_destination_available = nodes >= 2;
    capabilities->sched_getaffinity_ready = sched_getaffinity(0, sizeof(affinity), &affinity) == 0 &&
                                               CPU_COUNT(&affinity) > 0;
    /* Permission is verified later against the selected target; this only confirms the API is present. */
    capabilities->sched_setaffinity_ready = capabilities->sched_getaffinity_ready;
#ifdef SYS_move_pages
    capabilities->move_pages_available = true;
    capabilities->move_pages_ready = true;
#endif
    capabilities->thread_migration_ready = capabilities->cross_node_destination_available &&
                                             capabilities->sched_getaffinity_ready &&
                                             capabilities->sched_setaffinity_ready;
    capabilities->page_migration_ready = capabilities->cross_node_destination_available &&
                                            capabilities->move_pages_available && capabilities->move_pages_ready;
    capabilities->phase7_ready = capabilities->thread_migration_ready || capabilities->page_migration_ready;
    capabilities->production_real_migration_ready =
        environment_capabilities_production_real_migration_ready(capabilities);
    if (!capabilities->cross_node_destination_available) {
        capabilities->state = ENVIRONMENT_ENV_LIMITED;
        snprintf(capabilities->reason, sizeof(capabilities->reason), "fewer than two online NUMA nodes");
    } else if (!capabilities->sched_getaffinity_ready) {
        capabilities->state = ENVIRONMENT_ENV_LIMITED;
        snprintf(capabilities->reason, sizeof(capabilities->reason), "sched_getaffinity is unavailable");
    } else {
        capabilities->state = ENVIRONMENT_READY;
        snprintf(capabilities->reason, sizeof(capabilities->reason), "cross-node topology detected");
    }
    return true;
}
