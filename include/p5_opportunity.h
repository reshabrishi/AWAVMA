#ifndef AWAVMA_P5_OPPORTUNITY_H
#define AWAVMA_P5_OPPORTUNITY_H

#include "calibration.h"

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    P5_OPPORTUNITY_BENEFICIAL,
    P5_OPPORTUNITY_NOT_BENEFICIAL,
    P5_OPPORTUNITY_INSUFFICIENT_EVIDENCE,
    P5_OPPORTUNITY_CANDIDATE_UNAVAILABLE,
    P5_OPPORTUNITY_PLACEMENT_UNAVAILABLE,
    P5_OPPORTUNITY_GAIN_UNAVAILABLE,
    P5_OPPORTUNITY_COST_UNAVAILABLE,
    P5_OPPORTUNITY_DECISION_UNAVAILABLE,
    P5_OPPORTUNITY_STALE_IDENTITY,
    P5_OPPORTUNITY_UNSUPPORTED
} p5_opportunity_status_t;

typedef struct {
    const char *app_id;
    pid_t pid;
    uint64_t start_time_ticks;
    uint64_t temporal_generation;
    pid_t candidate_tid;
    uint64_t candidate_tid_start_time_ticks;
    bool candidate_available;
    bool candidate_verified;
    bool identity_match;
    bool generation_current;
    const char *classification;
    bool classification_available;
    bool classification_candidate_bound;
    const char *placement_relation;
    bool placement_available;
    size_t memory_total_pages;
    size_t memory_queryable_pages;
    int memory_dominant_node;
    int source_cpu;
    int source_node;
    bool source_available;
    cpu_set_t allowed_affinity;
    bool allowed_affinity_available;
    bool affinity_valid;
    int proposed_cpu;
    int proposed_node;
    bool destination_available;
    bool destination_permitted;
    bool expected_gain_available;
    double expected_gain_ms;
    bool migration_cost_available;
    double migration_cost_ms;
    bool decision_available;
    double memory_score;
    double thread_score;
    double epsilon;
} p5_opportunity_input_t;

typedef struct {
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    uint64_t temporal_generation;
    pid_t candidate_tid;
    uint64_t candidate_tid_start_time_ticks;
    char classification[16];
    bool classification_available;
    char placement_relation[16];
    bool placement_available;
    int source_cpu;
    int source_node;
    int proposed_cpu;
    int proposed_node;
    bool destination_available;
    size_t memory_total_pages;
    size_t memory_queryable_pages;
    int memory_dominant_node;
    double expected_gain_ms;
    bool expected_gain_available;
    double migration_cost_ms;
    double roi_margin_ms;
    bool roi_available;
    bool migration_cost_available;
    double decision_score;
    bool decision_available;
    p5_opportunity_status_t status;
    bool beneficial;
    char reason[160];
} p5_opportunity_result_t;

const char *p5_opportunity_status_name(p5_opportunity_status_t status);
/* Returns LOCAL, REMOTE, MIXED, or UNKNOWN from fully attributed owned-page evidence. */
const char *p5_opportunity_memory_relation(size_t total_pages, size_t queryable_pages,
                                           size_t unknown_pages, size_t dominant_pages,
                                           int dominant_node, int candidate_node);
bool p5_opportunity_select_destination(int source_cpu, int target_node,
                                       const cpu_set_t *allowed_affinity,
                                       const int cpu_nodes[CPU_SETSIZE],
                                       int *proposed_cpu, int *proposed_node);
int p5_opportunity_evaluate(const p5_opportunity_input_t *input,
                            p5_opportunity_result_t *result);
/* Reads a validated P4 match only; it never writes or recalibrates the artifact. */
bool p5_opportunity_calibration_evidence(const CalibrationSnapshot *snapshot,
                                         const CalibrationMatchRequest *request,
                                         double *expected_gain_pct,
                                          double *migration_cost_ms,
                                         calibration_match_status_t *status);
int p5_opportunity_write_state(const char *path, const p5_opportunity_result_t *result);

#endif
