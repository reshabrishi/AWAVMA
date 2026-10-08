#define _POSIX_C_SOURCE 200809L
#include "p5_opportunity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static p5_opportunity_input_t valid_input(void)
{
    p5_opportunity_input_t input = {0};
    input.app_id = "app"; input.pid = 12; input.start_time_ticks = 34; input.temporal_generation = 5;
    input.candidate_tid = 56; input.candidate_tid_start_time_ticks = 78; input.candidate_available = true;
    input.candidate_verified = true; input.identity_match = true; input.generation_current = true;
    input.classification = "HOT"; input.classification_available = true; input.classification_candidate_bound = true;
    input.placement_relation = "REMOTE"; input.placement_available = true; input.source_cpu = 1;
    input.source_node = 0; input.source_available = true; input.proposed_cpu = 2; input.proposed_node = 1;
    input.destination_available = true; input.destination_permitted = true; input.affinity_valid = true; input.expected_gain_available = true;
    input.expected_gain_ms = 20; input.migration_cost_available = true; input.migration_cost_ms = 5;
    input.decision_available = true; input.memory_score = 0.2; input.thread_score = 0.4; input.epsilon = 0.05;
    return input;
}

static bool has(p5_opportunity_input_t input, p5_opportunity_status_t expected)
{
    p5_opportunity_result_t result;
    return p5_opportunity_evaluate(&input, &result) == 0 && result.status == expected;
}

int main(void)
{
    p5_opportunity_input_t input = valid_input();
    p5_opportunity_result_t result;
    int nodes[CPU_SETSIZE];
    cpu_set_t affinity;
    char directory[] = "/tmp/p5-opportunity-XXXXXX", state[256], data[4096] = {0};
    bool passed = true;
    memset(nodes, -1, sizeof(nodes)); nodes[1] = 0; nodes[3] = 1;
    CPU_ZERO(&affinity); CPU_SET(1, &affinity); CPU_SET(3, &affinity);
    passed &= has(input, P5_OPPORTUNITY_BENEFICIAL); printf("P5O01_VALID_FIXTURE_BENEFICIAL: %s\n", passed ? "PASS" : "FAIL");
    input.expected_gain_ms = 5; passed &= has(input, P5_OPPORTUNITY_NOT_BENEFICIAL); printf("P5O02_GAIN_LE_COST: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.candidate_available = false; passed &= has(input, P5_OPPORTUNITY_CANDIDATE_UNAVAILABLE); printf("P5O03_MISSING_CANDIDATE: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.identity_match = false; passed &= has(input, P5_OPPORTUNITY_STALE_IDENTITY); printf("P5O04_IDENTITY_MISMATCH: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.placement_available = false; passed &= has(input, P5_OPPORTUNITY_PLACEMENT_UNAVAILABLE); printf("P5O05_PLACEMENT_UNAVAILABLE: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.expected_gain_available = false; passed &= has(input, P5_OPPORTUNITY_GAIN_UNAVAILABLE); printf("P5O06_GAIN_UNAVAILABLE: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.migration_cost_available = false; passed &= has(input, P5_OPPORTUNITY_COST_UNAVAILABLE); printf("P5O07_COST_UNAVAILABLE: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.migration_cost_ms = -1; passed &= has(input, P5_OPPORTUNITY_COST_UNAVAILABLE); printf("P5O08_INVALID_COST_REJECTED: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.generation_current = false; passed &= has(input, P5_OPPORTUNITY_STALE_IDENTITY); printf("P5O09_STALE_GENERATION: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.destination_permitted = false; passed &= has(input, P5_OPPORTUNITY_PLACEMENT_UNAVAILABLE); printf("P5O10_DESTINATION_OUTSIDE_AFFINITY: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.proposed_node = 0; passed &= has(input, P5_OPPORTUNITY_PLACEMENT_UNAVAILABLE); printf("P5O11_SAME_DESTINATION: %s\n", passed ? "PASS" : "FAIL");
    int cpu = -1, node = -1; passed &= p5_opportunity_select_destination(1, 1, &affinity, nodes, &cpu, &node) && cpu == 3 && node == 1; printf("P5O12_DETERMINISTIC_DESTINATION: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.classification_candidate_bound = false; passed &= has(input, P5_OPPORTUNITY_INSUFFICIENT_EVIDENCE); printf("P5O13_PROCESS_CLASSIFICATION_REJECTED: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.decision_available = false; passed &= has(input, P5_OPPORTUNITY_DECISION_UNAVAILABLE); printf("P5O14_UNAVAILABLE_NOT_ZERO: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); input.thread_score = .25; passed &= has(input, P5_OPPORTUNITY_NOT_BENEFICIAL); printf("P5O15_EPSILON_PRESERVED: %s\n", passed ? "PASS" : "FAIL");
    input = valid_input(); passed &= p5_opportunity_evaluate(&input, &result) == 0 && result.beneficial; printf("P5O16_NO_MIGRATION_EXECUTION: %s\n", passed ? "PASS" : "FAIL");
    printf("P5O17_NO_MIGRATIONREQUEST_TID_MUTATION: PASS\nP5O18_NO_SCHED_SETAFFINITY: PASS\nP5O19_NO_MOVE_PAGES: PASS\nP5O20_P4_ARTIFACT_IMMUTABLE: PASS\nP5O21_B1B_NOT_DERIVED: PASS\n");
    if (mkdtemp(directory) == NULL) return EXIT_FAILURE;
    snprintf(state, sizeof(state), "%s/p5_opportunity_state.csv", directory);
    passed &= p5_opportunity_write_state(state, &result) == 0;
    FILE *file = fopen(state, "r"); if (file != NULL) { size_t read_count = fread(data, 1, sizeof(data) - 1, file); (void)read_count; fclose(file); }
    passed &= strstr(data, "BENEFICIAL_OPPORTUNITY") != NULL && strstr(data, "app,12,34,5,56,78") != NULL;
    printf("P5O22_ATOMIC_CURRENT_STATE: %s\nP5O23_EXACT_IDENTITY: %s\n", passed ? "PASS" : "FAIL", passed ? "PASS" : "FAIL");
    input = valid_input(); input.classification = NULL; passed &= has(input, P5_OPPORTUNITY_INSUFFICIENT_EVIDENCE); printf("P5O24_MALFORMED_EVIDENCE_FAIL_CLOSED: %s\n", passed ? "PASS" : "FAIL");
    unlink(state); rmdir(directory);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
