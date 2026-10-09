#include "p5_c2a_collector.h"

#include <stdio.h>

static void set_reason(char *reason, size_t size, const char *value)
{
    if (reason != NULL && size != 0) snprintf(reason, size, "%s", value);
}

bool p5_c2a_collector_select_topology(const P5C2ACollectorNode *nodes, size_t count,
                                      P5C2ACollectorTopology *topology)
{
    int local = -1, remote = -1, distance = -1;
    if (nodes == NULL || topology == NULL) return false;
    for (size_t i = 0; i < count; ++i)
        if (nodes[i].permitted_cpus != 0 && (local < 0 || nodes[i].node < local)) local = nodes[i].node;
    if (local < 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (nodes[i].permitted_cpus == 0 || nodes[i].node == local) continue;
        if (nodes[i].distance_from_local > distance ||
            (nodes[i].distance_from_local == distance && (remote < 0 || nodes[i].node < remote))) {
            remote = nodes[i].node;
            distance = nodes[i].distance_from_local;
        }
    }
    if (remote < 0) return false;
    *topology = (P5C2ACollectorTopology){local, remote, distance};
    return true;
}

size_t p5_c2a_collector_select_physical_cores(const P5C2ACollectorCpu *cpus, size_t count,
                                               int node, int *selected_cpus, size_t capacity)
{
    size_t selected = 0;
    if (cpus == NULL || selected_cpus == NULL) return 0;
    /* The lowest logical CPU is the stable representative for each physical core. */
    for (size_t i = 0; i < count && selected < capacity; ++i) {
        bool duplicate = false;
        if (!cpus[i].permitted || cpus[i].node != node) continue;
        for (size_t j = 0; j < count; ++j)
            if (cpus[j].permitted && cpus[j].node == node && cpus[j].package_id == cpus[i].package_id &&
                cpus[j].core_id == cpus[i].core_id && cpus[j].cpu < cpus[i].cpu) duplicate = true;
        if (!duplicate) selected_cpus[selected++] = cpus[i].cpu;
    }
    return selected;
}

size_t p5_c2a_collector_matrix_a(P5C2ACollectorMatrixEntry *entries, size_t capacity)
{
    static const P5C2ACollectorMatrixEntry matrix[] = {
        {P5_THREAD_ACTIVITY_PROFILE_LOW, 10},
        {P5_THREAD_ACTIVITY_PROFILE_MID, 40},
        {P5_THREAD_ACTIVITY_PROFILE_HIGH, 100}
    };
    if (entries == NULL || capacity < sizeof(matrix) / sizeof(matrix[0])) return 0;
    for (size_t i = 0; i < sizeof(matrix) / sizeof(matrix[0]); ++i) entries[i] = matrix[i];
    return sizeof(matrix) / sizeof(matrix[0]);
}

const char *p5_c2a_collector_evidence_label_name(P5C2AEvidenceLabel label)
{
    return label == P5_C2A_EVIDENCE_BASELINE ? "BASELINE" : label == P5_C2A_EVIDENCE_DELTA ? "DELTA" : "INVALID";
}

bool p5_c2a_collector_label_delta(uint64_t previous, uint64_t current,
                                  P5C2AEvidenceLabel *label, uint64_t *delta)
{
    if (label == NULL || delta == NULL || current < previous) return false;
    *label = previous == 0 ? P5_C2A_EVIDENCE_BASELINE : P5_C2A_EVIDENCE_DELTA;
    *delta = previous == 0 ? current : current - previous;
    return true;
}

bool p5_c2a_collector_run_valid(const P5C2ARunPlan *plan, char *reason, size_t reason_size)
{
    if (plan == NULL || !plan->topology_valid) { set_reason(reason, reason_size, "topology_unavailable"); return false; }
    if (!plan->physical_cores_valid) { set_reason(reason, reason_size, "physical_cores_unavailable"); return false; }
    if (!plan->source_placement_verified) { set_reason(reason, reason_size, "source_placement_unverified"); return false; }
    if (!plan->numa_balancing_observed) { set_reason(reason, reason_size, "numa_balancing_unobserved"); return false; }
    if (plan->numa_balancing_restoration_planned && !plan->numa_balancing_restored) { set_reason(reason, reason_size, "numa_balancing_not_restored"); return false; }
    if (!plan->worker_evidence_available) { set_reason(reason, reason_size, "worker_evidence_unavailable"); return false; }
    set_reason(reason, reason_size, "valid");
    return true;
}
