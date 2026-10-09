#ifndef AWAVMA_P5_C2A_COLLECTOR_H
#define AWAVMA_P5_C2A_COLLECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "p5_thread_activity_calibration.h"

#define P5_C2A_COLLECTOR_MATRIX_A_VERSION "matrix-a-v1"
#define P5_C2A_COLLECTOR_ENV_LIMITED 3

typedef struct {
    int node;
    unsigned permitted_cpus;
    int distance_from_local;
} P5C2ACollectorNode;

typedef struct {
    int local_node;
    int remote_node;
    int distance;
} P5C2ACollectorTopology;

typedef struct {
    int cpu;
    int node;
    int package_id;
    int core_id;
    bool permitted;
} P5C2ACollectorCpu;

typedef struct {
    P5ThreadActivityProfile profile;
    unsigned intensity_percent;
} P5C2ACollectorMatrixEntry;

typedef enum {
    P5_C2A_EVIDENCE_BASELINE,
    P5_C2A_EVIDENCE_DELTA
} P5C2AEvidenceLabel;

typedef struct {
    bool topology_valid;
    bool physical_cores_valid;
    bool source_placement_verified;
    bool numa_balancing_observed;
    bool numa_balancing_restoration_planned;
    bool numa_balancing_restored;
    bool worker_evidence_available;
} P5C2ARunPlan;

bool p5_c2a_collector_select_topology(const P5C2ACollectorNode *nodes, size_t count,
                                      P5C2ACollectorTopology *topology);
size_t p5_c2a_collector_select_physical_cores(const P5C2ACollectorCpu *cpus, size_t count,
                                               int node, int *selected_cpus, size_t capacity);
size_t p5_c2a_collector_matrix_a(P5C2ACollectorMatrixEntry *entries, size_t capacity);
const char *p5_c2a_collector_evidence_label_name(P5C2AEvidenceLabel label);
bool p5_c2a_collector_label_delta(uint64_t previous, uint64_t current,
                                  P5C2AEvidenceLabel *label, uint64_t *delta);
bool p5_c2a_collector_run_valid(const P5C2ARunPlan *plan, char *reason, size_t reason_size);

#endif
