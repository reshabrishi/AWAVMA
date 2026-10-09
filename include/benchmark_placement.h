#ifndef AWAVMA_BENCHMARK_PLACEMENT_H
#define AWAVMA_BENCHMARK_PLACEMENT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BENCHMARK_PLACEMENT_DEFAULT,
    BENCHMARK_PLACEMENT_LOCAL,
    BENCHMARK_PLACEMENT_REMOTE
} benchmark_placement_mode_t;

typedef struct {
    int node;
    unsigned permitted_cpus;
    int distance_from_local;
} benchmark_placement_node_t;

typedef struct {
    int local_node;
    int remote_node;
    int numa_distance;
    unsigned permitted_node_count;
} benchmark_placement_topology_t;

typedef struct {
    benchmark_placement_mode_t mode;
    int local_node;
    int remote_node;
    int requested_memory_node;
    int numa_distance;
    size_t total_pages;
    size_t queryable_pages;
    size_t expected_node_pages;
    size_t local_pages;
    size_t remote_pages;
    size_t other_pages;
    size_t unknown_pages;
    int observed_dominant_node;
    bool memory_policy_restored;
    char verification_status[32];
    char verification_reason[160];
} benchmark_placement_evidence_t;

const char *benchmark_placement_mode_name(benchmark_placement_mode_t mode);
bool benchmark_placement_select(const benchmark_placement_node_t *nodes, size_t count,
                                benchmark_placement_topology_t *topology);
int benchmark_placement_discover(benchmark_placement_topology_t *topology);
int benchmark_placement_prepare(benchmark_placement_mode_t mode,
                                const benchmark_placement_topology_t *topology,
                                void *allocation, size_t bytes,
                                 benchmark_placement_evidence_t *evidence);
/* Read-only end-of-run residency gate for controlled placement. */
int benchmark_placement_verify(const benchmark_placement_topology_t *topology, void *allocation,
                               size_t bytes, benchmark_placement_evidence_t *evidence);
int benchmark_placement_write(const char *path, const benchmark_placement_evidence_t *evidence);

#endif
