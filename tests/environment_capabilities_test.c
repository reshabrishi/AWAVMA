#include "environment_capabilities.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    EnvironmentCapabilities capabilities;

    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.online_numa_nodes = 2;
    capabilities.thread_migration_ready = true;
    capabilities.page_migration_ready = true;
    capabilities.phase7_ready = true;
    if (!environment_capabilities_production_real_migration_ready(&capabilities))
        return 1;
    capabilities.thread_migration_ready = false;
    if (environment_capabilities_production_real_migration_ready(&capabilities))
        return 1;
    capabilities.thread_migration_ready = true;
    capabilities.page_migration_ready = false;
    if (environment_capabilities_production_real_migration_ready(&capabilities))
        return 1;
    puts("environment_capabilities_test: PASS");
    return 0;
}
