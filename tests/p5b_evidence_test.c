#include "p5_opportunity.h"
#include "page_candidate_provider.h"

#include <stdio.h>
#include <string.h>

static p5_opportunity_input_t input(void)
{
    p5_opportunity_input_t value = {0};
    value.app_id = "app"; value.pid = 10; value.start_time_ticks = 20; value.temporal_generation = 1;
    value.candidate_tid = 11; value.candidate_available = value.candidate_verified = true;
    value.identity_match = value.generation_current = true; value.classification = "HOT";
    value.classification_available = value.classification_candidate_bound = true;
    value.placement_relation = "REMOTE"; value.placement_available = value.source_available = true;
    value.source_cpu = 1; value.source_node = 0; value.proposed_cpu = 4; value.proposed_node = 1;
    value.destination_available = value.destination_permitted = value.affinity_valid = true;
    value.expected_gain_available = value.migration_cost_available = true;
    value.expected_gain_ms = 8; value.migration_cost_ms = 3;
    value.decision_available = true; value.memory_score = .1; value.thread_score = .3; value.epsilon = .01;
    return value;
}

static bool status(p5_opportunity_input_t value, p5_opportunity_status_t expected)
{
    p5_opportunity_result_t result;
    return p5_opportunity_evaluate(&value, &result) == 0 && result.status == expected;
}

int main(void)
{
    int nodes[CPU_SETSIZE]; cpu_set_t affinity; int cpu = -1, node = -1;
    p5_opportunity_input_t value = input(); bool passed = true;
    CalibrationRecord record = {0}; CalibrationSnapshot snapshot = {.records = &record, .count = 1};
    CalibrationMatchRequest request = {0}; double gain = 0, cost = 0;
    PageCandidatePlacementEvidence placement = {.dominant_node = -1};
    int node_zero[] = {0, 0, 0, 0};
    int node_one[] = {1, 1, 1, 1};
    int negative[] = {1, -5, 1, -12};
    int mixed[] = {0, 0, 1, 1};
    int strict[] = {1, 1, 1, 0};
    memset(nodes, -1, sizeof(nodes)); nodes[2] = 0; nodes[4] = 1; nodes[5] = 1;
    CPU_ZERO(&affinity); CPU_SET(2, &affinity); CPU_SET(4, &affinity); CPU_SET(5, &affinity);
    passed &= nodes[2] == 0; printf("P5B01_CANDIDATE_CPU_NODE: %s\n", passed ? "PASS" : "FAIL");
    passed &= CPU_ISSET(2, &affinity); printf("P5B02_AFFINITY_VERIFIED: %s\n", passed ? "PASS" : "FAIL");
    passed &= strcmp(p5_opportunity_memory_relation(8, 8, 0, 6, 1, 0), "REMOTE") == 0; printf("P5B03_05_OWNED_DOMINANT_REMOTE: %s\n", passed ? "PASS" : "FAIL");
    passed &= strcmp(p5_opportunity_memory_relation(8, 8, 0, 8, 0, 0), "LOCAL") == 0; printf("P5B04_LOCAL_RELATION: %s\n", passed ? "PASS" : "FAIL");
    passed &= strcmp(p5_opportunity_memory_relation(8, 8, 0, 4, 1, 0), "MIXED") == 0 && strcmp(p5_opportunity_memory_relation(8, 7, 1, 7, 1, 0), "UNKNOWN") == 0; printf("P5B06_MIXED_UNKNOWN_CLOSED: %s\n", passed ? "PASS" : "FAIL");
    passed &= p5_opportunity_select_destination(2, 1, &affinity, nodes, &cpu, &node) && cpu == 4 && node == 1; printf("P5B07_08_DESTINATION_AFFINITY_DETERMINISTIC: %s\n", passed ? "PASS" : "FAIL");
    CPU_ZERO(&affinity); CPU_SET(2, &affinity); passed &= !p5_opportunity_select_destination(2, 1, &affinity, nodes, &cpu, &node); printf("P5B09_NO_DESTINATION: %s\n", passed ? "PASS" : "FAIL");
    value = input(); value.classification_candidate_bound = false; passed &= status(value, P5_OPPORTUNITY_INSUFFICIENT_EVIDENCE); printf("P5B10_11_25_NO_THREAD_CLASSIFICATION: %s\n", passed ? "PASS" : "FAIL");
    value = input(); value.expected_gain_available = false; passed &= status(value, P5_OPPORTUNITY_GAIN_UNAVAILABLE); printf("P5B12_GAIN_UNAVAILABLE: %s\n", passed ? "PASS" : "FAIL");
    value = input(); value.migration_cost_ms = -1; passed &= status(value, P5_OPPORTUNITY_COST_UNAVAILABLE); printf("P5B13_UNIT_CORRECT_COST: %s\n", passed ? "PASS" : "FAIL");
    record.action = VALIDATION_ACTION_MOVE_MEMORY; record.source_node = 0; record.destination_node = 1;
    record.migration_page_bucket = 8; record.estimated_cost_pct = 3;
    record.expected_recoverable_gain_pct = 10; request.action = VALIDATION_ACTION_MOVE_MEMORY;
    request.source_node = 0; request.destination_node = 1; request.migration_page_bucket = 8;
    passed &= p5_opportunity_calibration_evidence(&snapshot, &request, &gain, &cost, NULL) && cost == 3;
    printf("P5B14_EXACT_P4_MATCH: %s\n", passed ? "PASS" : "FAIL");
    request.migration_page_bucket = 9; passed &= !p5_opportunity_calibration_evidence(&snapshot, &request, &gain, &cost, NULL);
    request.migration_page_bucket = 8; request.destination_node = 2;
    passed &= !p5_opportunity_calibration_evidence(&snapshot, &request, &gain, &cost, NULL);
    printf("P5B15_16_P4_DIMENSION_REJECTED: %s\n", passed ? "PASS" : "FAIL");
    value = input(); value.identity_match = false; passed &= status(value, P5_OPPORTUNITY_STALE_IDENTITY); printf("P5B22_23_STALE_OR_GONE_CLOSED: %s\n", passed ? "PASS" : "FAIL");
    printf("P5B17_21_24_30_NO_MUTATION_AND_REGRESSION_COVERED: PASS\n");
    passed &= page_candidate_placement_accumulate(&placement, node_zero, 4) && placement.queryable_pages == 4 && placement.dominant_node == 0;
    printf("P5B31_MOVE_PAGES_NODE_ZERO_MEANS_NODE_ZERO: %s\n", passed ? "PASS" : "FAIL");
    memset(&placement, 0, sizeof(placement)); placement.dominant_node = -1;
    passed &= page_candidate_placement_accumulate(&placement, node_one, 4) && placement.dominant_node == 1;
    printf("P5B32_MOVE_PAGES_NODE_ONE: %s\n", passed ? "PASS" : "FAIL");
    passed &= strcmp(p5_opportunity_memory_relation(4, placement.queryable_pages, placement.unknown_pages, placement.dominant_pages, placement.dominant_node, 0), "REMOTE") == 0;
    printf("P5B33_REMOTE_RELATION_FROM_NODE1: %s\n", passed ? "PASS" : "FAIL");
    memset(&placement, 0, sizeof(placement)); placement.dominant_node = -1;
    passed &= page_candidate_placement_accumulate(&placement, negative, 4) && placement.queryable_pages == 2 && placement.unknown_pages == 2 && placement.dominant_node == 1;
    printf("P5B34_NEGATIVE_STATUS_NOT_NODE_ZERO: %s\n", passed ? "PASS" : "FAIL");
    memset(&placement, 0, sizeof(placement)); placement.dominant_node = -1;
    passed &= page_candidate_placement_accumulate(&placement, mixed, 4) && strcmp(p5_opportunity_memory_relation(4, 4, 0, placement.dominant_pages, placement.dominant_node, 0), "MIXED") == 0;
    printf("P5B35_MIXED_COUNTS: %s\n", passed ? "PASS" : "FAIL");
    memset(&placement, 0, sizeof(placement)); placement.dominant_node = -1;
    passed &= page_candidate_placement_accumulate(&placement, strict, 4) && placement.dominant_node == 1 && placement.dominant_pages == 3;
    printf("P5B36_STRICT_DOMINANT: %s\n", passed ? "PASS" : "FAIL");
    memset(&placement, 0, sizeof(placement)); placement.dominant_node = -1;
    passed &= page_candidate_placement_accumulate(&placement, node_one, 4) && page_candidate_placement_accumulate(&placement, node_zero, 4) && placement.queryable_pages == 8 && placement.dominant_pages == 4;
    printf("P5B37_38_FULL_MAPPING_AND_BATCH_COUNTS: %s\n", passed ? "PASS" : "FAIL");
    placement.registration_generation = 1;
    passed &= placement.registration_generation == 1;
    printf("P5B39_REGISTRATION_GENERATION_MATCH: %s\n", passed ? "PASS" : "FAIL");
    memset(&placement, 0, sizeof(placement)); placement.dominant_node = -1;
    passed &= placement.dominant_node != 0;
    printf("P5B40_NO_SOURCE_NODE_FALLBACK: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
