#include "benchmark_placement.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    benchmark_placement_topology_t topology;
    benchmark_placement_evidence_t evidence = {
        .mode = BENCHMARK_PLACEMENT_REMOTE,
        .local_node = 4,
        .requested_memory_node = 7,
        .numa_distance = 21,
        .total_pages = 16,
        .queryable_pages = 16,
        .expected_node_pages = 16,
        .local_pages = 0,
        .remote_pages = 16,
        .memory_policy_restored = true
    };
    benchmark_placement_node_t nodes[] = {
        {.node = 4, .permitted_cpus = 2, .distance_from_local = 10},
        {.node = 7, .permitted_cpus = 1, .distance_from_local = 21},
        {.node = 9, .permitted_cpus = 1, .distance_from_local = 21},
        {.node = 2, .permitted_cpus = 0, .distance_from_local = 99}
    };

    assert(benchmark_placement_select(nodes, 4, &topology));
    assert(topology.local_node == 4 && topology.remote_node == 7 && topology.numa_distance == 21);
    nodes[0].permitted_cpus = 0;
    assert(benchmark_placement_select(nodes, 4, &topology));
    assert(topology.local_node == 7 && topology.remote_node == 9);
    nodes[2].permitted_cpus = 0;
    assert(!benchmark_placement_select(nodes, 4, &topology));

    char artifact[128], temporary[132], contents[1024] = {0};
    snprintf(artifact, sizeof(artifact), "/tmp/awavma-placement-%ld.csv", (long)getpid());
    snprintf(temporary, sizeof(temporary), "%s.tmp", artifact);
    snprintf(evidence.verification_status, sizeof(evidence.verification_status), "PASS");
    snprintf(evidence.verification_reason, sizeof(evidence.verification_reason), "all pages verified on requested node");
    assert(benchmark_placement_write(artifact, &evidence) == 0);
    assert(access(temporary, F_OK) != 0);
    FILE *file = fopen(artifact, "r");
    assert(file != NULL);
    assert(fread(contents, 1, sizeof(contents) - 1, file) > 0);
    assert(fclose(file) == 0);
    assert(strstr(contents, "placement_mode,local_node,requested_memory_node") != NULL);
    assert(strstr(contents, "remote,4,7,21,16,16,16,0,16,0,0,1.000000000") != NULL);
    assert(strstr(contents, "address") == NULL);
    assert(unlink(artifact) == 0);
    printf("benchmark_placement_test: PASS\n");
    return 0;
}
