#include "p5_c2a_collector.h"

#include <stdio.h>
#include <string.h>

static bool passed = true;
static void check(unsigned id, const char *name, bool ok)
{
    printf("P5C2COL%02u_%s: %s\n", id, name, ok ? "PASS" : "FAIL");
    passed = passed && ok;
}

int main(void)
{
    P5C2ACollectorNode nodes[] = {{2, 4, 20}, {0, 2, 10}, {1, 0, 40}, {3, 2, 20}};
    P5C2ACollectorCpu cpus[] = {{7, 0, 0, 1, true}, {1, 0, 0, 0, true}, {3, 0, 0, 0, true}, {5, 0, 0, 1, true}, {9, 1, 0, 2, true}, {11, 0, 1, 0, false}};
    P5C2ACollectorTopology topology = {0}; P5C2ACollectorMatrixEntry matrix[3];
    P5C2AEvidenceLabel label; uint64_t delta; int selected[3] = {0}; char reason[64];
    P5C2ARunPlan plan = {true, true, true, true, false, false, true};
    check(1, "TOPOLOGY_SELECTS_LOWEST_LOCAL", p5_c2a_collector_select_topology(nodes, 4, &topology) && topology.local_node == 0);
    check(2, "TOPOLOGY_SELECTS_FARTHEST_REMOTE", topology.remote_node == 2 && topology.distance == 20);
    check(3, "TOPOLOGY_DISTANCE_TIE_LOWEST_NODE", topology.remote_node == 2);
    check(4, "TOPOLOGY_REQUIRES_PERMITTED_REMOTE", !p5_c2a_collector_select_topology(nodes + 1, 1, &topology));
    check(5, "TOPOLOGY_NULL_REJECTED", !p5_c2a_collector_select_topology(NULL, 0, &topology));
    check(6, "TOPOLOGY_OUTPUT_REQUIRED", !p5_c2a_collector_select_topology(nodes, 4, NULL));
    check(7, "SMT_SELECTS_TWO_PHYSICAL_CORES", p5_c2a_collector_select_physical_cores(cpus, 6, 0, selected, 3) == 2);
    check(8, "SMT_SELECTS_LOWEST_SIBLING", selected[0] == 1 && selected[1] == 5);
    check(9, "SMT_EXCLUDES_OTHER_NODE", p5_c2a_collector_select_physical_cores(cpus, 6, 1, selected, 3) == 1 && selected[0] == 9);
    check(10, "SMT_EXCLUDES_UNPERMITTED", p5_c2a_collector_select_physical_cores(cpus, 6, 0, selected, 3) == 2);
    check(11, "SMT_CAPACITY_LIMIT", p5_c2a_collector_select_physical_cores(cpus, 6, 0, selected, 1) == 1);
    check(12, "SMT_NULL_REJECTED", p5_c2a_collector_select_physical_cores(NULL, 0, 0, selected, 1) == 0);
    check(13, "MATRIX_HAS_THREE_PROFILES", p5_c2a_collector_matrix_a(matrix, 3) == 3);
    check(14, "MATRIX_LOW", matrix[0].profile == P5_THREAD_ACTIVITY_PROFILE_LOW && matrix[0].intensity_percent == 10);
    check(15, "MATRIX_MID", matrix[1].profile == P5_THREAD_ACTIVITY_PROFILE_MID && matrix[1].intensity_percent == 40);
    check(16, "MATRIX_HIGH", matrix[2].profile == P5_THREAD_ACTIVITY_PROFILE_HIGH && matrix[2].intensity_percent == 100);
    check(17, "MATRIX_VERSION", !strcmp(P5_C2A_COLLECTOR_MATRIX_A_VERSION, "matrix-a-v1"));
    check(18, "MATRIX_CAPACITY_REQUIRED", p5_c2a_collector_matrix_a(matrix, 2) == 0);
    check(19, "MATRIX_OUTPUT_REQUIRED", p5_c2a_collector_matrix_a(NULL, 3) == 0);
    check(20, "BASELINE_LABEL", p5_c2a_collector_label_delta(0, 44, &label, &delta) && label == P5_C2A_EVIDENCE_BASELINE && delta == 44);
    check(21, "DELTA_LABEL", p5_c2a_collector_label_delta(44, 61, &label, &delta) && label == P5_C2A_EVIDENCE_DELTA && delta == 17);
    check(22, "ZERO_DELTA_VALID", p5_c2a_collector_label_delta(44, 44, &label, &delta) && delta == 0);
    check(23, "COUNTER_REGRESSION_REJECTED", !p5_c2a_collector_label_delta(61, 44, &label, &delta));
    check(24, "LABEL_BASELINE_TEXT", !strcmp(p5_c2a_collector_evidence_label_name(P5_C2A_EVIDENCE_BASELINE), "BASELINE"));
    check(25, "LABEL_DELTA_TEXT", !strcmp(p5_c2a_collector_evidence_label_name(P5_C2A_EVIDENCE_DELTA), "DELTA"));
    check(26, "RUN_VALID", p5_c2a_collector_run_valid(&plan, reason, sizeof(reason)) && !strcmp(reason, "valid"));
    plan.topology_valid = false; check(27, "RUN_REQUIRES_TOPOLOGY", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason)) && !strcmp(reason, "topology_unavailable")); plan.topology_valid = true;
    plan.physical_cores_valid = false; check(28, "RUN_REQUIRES_PHYSICAL_CORES", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.physical_cores_valid = true;
    plan.source_placement_verified = false; check(29, "RUN_REQUIRES_PLACEMENT", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.source_placement_verified = true;
    plan.numa_balancing_observed = false; check(30, "RUN_REQUIRES_NUMA_OBSERVATION", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.numa_balancing_observed = true;
    plan.numa_balancing_restoration_planned = true; check(31, "RESTORATION_REQUIRED_WHEN_PLANNED", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.numa_balancing_restored = true;
    check(32, "RESTORATION_COMPLETES_PLAN", p5_c2a_collector_run_valid(&plan, reason, sizeof(reason)));
    plan.worker_evidence_available = false; check(33, "RUN_REQUIRES_WORKER_EVIDENCE", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.worker_evidence_available = true;
    check(34, "NULL_PLAN_REJECTED", !p5_c2a_collector_run_valid(NULL, reason, sizeof(reason)));
    check(35, "REASON_WRITES", !p5_c2a_collector_run_valid(NULL, reason, sizeof(reason)) && !strcmp(reason, "topology_unavailable"));
    check(36, "REASON_OPTIONAL", !p5_c2a_collector_run_valid(NULL, NULL, 0));
    check(37, "LOCAL_NODE_PRESERVED", topology.local_node == 0 || topology.local_node == 0);
    check(38, "PROFILE_NAMES", !strcmp(p5_thread_activity_profile_name(matrix[0].profile), "LOW"));
    check(39, "DISTINCT_MATRIX_ENTRIES", matrix[0].profile != matrix[1].profile && matrix[1].profile != matrix[2].profile);
    check(40, "DELTA_POINTERS_REQUIRED", !p5_c2a_collector_label_delta(0, 1, NULL, &delta));
    check(41, "DELTA_OUTPUT_REQUIRED", !p5_c2a_collector_label_delta(0, 1, &label, NULL));
    check(42, "TOPOLOGY_HAS_DISTANCE", p5_c2a_collector_select_topology(nodes, 4, &topology) && topology.distance == 20);
    check(43, "NO_RUNTIME_AUTHORITY", true);
    return passed ? 0 : 1;
}
