#include "environment_capabilities.h"

#include <stdio.h>

int main(void)
{
    EnvironmentCapabilities capabilities;

    if (!environment_capabilities_detect(&capabilities))
        return 2;
    printf("state=%s numa_nodes=%zu thread_ready=%s page_ready=%s phase7_ready=%s production_real_migration_ready=%s reason=%s\n",
           environment_capability_state_name(capabilities.state), capabilities.online_numa_nodes,
           capabilities.thread_migration_ready ? "true" : "false",
           capabilities.page_migration_ready ? "true" : "false",
           capabilities.phase7_ready ? "true" : "false",
           capabilities.production_real_migration_ready ? "true" : "false",
           capabilities.reason);
    return capabilities.state == ENVIRONMENT_READY ? 0 : 3;
}
