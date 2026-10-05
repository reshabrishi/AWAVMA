#include "benefit_classifier.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BenefitClassifierInput input_for(void)
{
    static DecisionData phase5;
    static ValidationResult phase6;
    static MigrationTarget target;
    BenefitClassifierInput input;

    memset(&phase5, 0, sizeof(phase5));
    memset(&phase6, 0, sizeof(phase6));
    memset(&target, 0, sizeof(target));
    phase5.pid = 42;
    phase5.action = VALIDATION_ACTION_MOVE_THREAD;
    phase5.gain_available = true;
    phase5.cost_available = true;
    phase5.evidence_model = DECISION_EVIDENCE_EMPIRICAL_GAIN_COST;
    phase5.predicted_gain = 80.0;
    phase5.estimated_cost = 20.0;
    phase6.pid = 42;
    phase6.action = VALIDATION_ACTION_MOVE_THREAD;
    phase6.confidence_status = GATE_PASS;
    phase6.roi_status = GATE_PASS;
    phase6.confidence_score = 80.0;
    phase6.roi_score = 20.0;
    phase6.safety_status = GATE_PASS;
    phase6.safety_score = 80.0;
    snprintf(phase6.final_decision, sizeof(phase6.final_decision), "APPROVED");
    target.pid = 42;
    target.start_time_ticks = 99;
    target.action = VALIDATION_ACTION_MOVE_THREAD;
    snprintf(target.attempt_id, sizeof(target.attempt_id), "attempt-1");
    target.source_node_known = true;
    target.source_numa_node = 0;
    target.has_target_numa_node = true;
    target.target_numa_node = 1;
    target.has_target_cpu_mask = true;
    CPU_SET(2, &target.target_cpu_mask);
    memset(&input, 0, sizeof(input));
    input.pid = 42;
    input.start_time_ticks = 99;
    input.attempt_id = "attempt-1";
    input.action = VALIDATION_ACTION_MOVE_THREAD;
    input.process_active = true;
    input.identity_match = true;
    input.phase5_fresh = true;
    input.phase6_fresh = true;
    input.evidence_attempt_bound = true;
    input.decision = &phase5;
    input.validation = &phase6;
    input.target = &target;
    input.target_provider_validated = true;
    input.target_online = true;
    input.target_permitted = true;
    input.source_known = true;
    input.source_target_valid = true;
    input.calibration.state = BENEFIT_CALIBRATION_VALIDATED_TEST_ONLY;
    input.calibration.provenance = "synthetic-test-only";
    return input;
}

static bool expect(const char *id, BenefitClassifierInput *input, BenefitClassification expected)
{
    BenefitDecision decision;
    BenefitClassification actual = benefit_classifier_evaluate(input, &decision);
    bool passed = actual == expected && decision.classification == expected &&
                  decision.pid == input->pid && decision.start_time_ticks == input->start_time_ticks &&
                  strcmp(decision.attempt_id, input->attempt_id) == 0;

    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
    return passed;
}

int main(void)
{
    BenefitClassifierInput input;
    bool passed = true;

    input = input_for();
    passed = expect("BC01_SYNTHETIC_VALID_THREAD", &input, BENEFIT_SUPPORTED) && passed;
    input = input_for(); ((DecisionData *)input.decision)->gain_available = false;
    passed = expect("BC02_MISSING_BENEFIT_EVIDENCE", &input, INSUFFICIENT_BENEFIT_EVIDENCE) && passed;
    input = input_for(); ((ValidationResult *)input.validation)->confidence_status = GATE_INVALID;
    passed = expect("BC03_MISSING_CONFIDENCE", &input, INSUFFICIENT_BENEFIT_EVIDENCE) && passed;
    input = input_for(); ((ValidationResult *)input.validation)->roi_status = GATE_FAIL;
    passed = expect("BC04_INSUFFICIENT_ROI", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); input.identity_match = false;
    passed = expect("BC05_STALE_IDENTITY", &input, STALE_OR_IDENTITY_MISMATCH) && passed;
    input = input_for(); snprintf(((MigrationTarget *)input.target)->attempt_id, 128, "other");
    passed = expect("BC06_ATTEMPT_ID_MISMATCH", &input, TARGET_NOT_VALIDATED) && passed;
    input = input_for(); ((MigrationTarget *)input.target)->pid = 43;
    passed = expect("BC07_TARGET_ID_MISMATCH", &input, TARGET_NOT_VALIDATED) && passed;
    input = input_for(); input.target_provider_validated = false;
    passed = expect("BC08_TARGET_NOT_VALIDATED", &input, TARGET_NOT_VALIDATED) && passed;
    input = input_for(); input.target_online = false;
    passed = expect("BC09_OFFLINE_DESTINATION", &input, TARGET_NOT_VALIDATED) && passed;
    input = input_for(); input.target_permitted = false;
    passed = expect("BC10_OUTSIDE_ALLOWED_AFFINITY", &input, TARGET_NOT_VALIDATED) && passed;
    input = input_for(); input.source_known = false;
    passed = expect("BC11_SOURCE_UNKNOWN", &input, TARGET_NOT_VALIDATED) && passed;
    input = input_for(); input.source_target_valid = false;
    passed = expect("BC12_SOURCE_AMBIGUOUS", &input, TARGET_NOT_VALIDATED) && passed;
    input = input_for(); input.cooldown_active = true;
    passed = expect("BC13_COOLDOWN", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); input.quarantined = true;
    passed = expect("BC14_QUARANTINE", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); input.history_suppressed = true;
    passed = expect("BC15_HISTORY_SUPPRESSION", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); input.action = VALIDATION_ACTION_MOVE_MEMORY;
    passed = expect("BC16_PAGE_RECOVERY_REQUIRED", &input, PAGE_RECOVERY_REQUIRED) && passed;
    input = input_for(); input.calibration.state = BENEFIT_CALIBRATION_UNAVAILABLE;
    passed = expect("BC17_UNCALIBRATED_PRODUCTION", &input, BENEFIT_POLICY_UNCALIBRATED) && passed;
    input = input_for(); input.phase5_fresh = false;
    passed = expect("BC18_STALE_PHASE5_PHASE6", &input, STALE_OR_IDENTITY_MISMATCH) && passed;
    input = input_for(); input.action = VALIDATION_ACTION_NO_MIGRATION;
    passed = expect("BC19_NO_MIGRATION_ACTION", &input, ACTION_NOT_ELIGIBLE) && passed;
    DecisionData margins = {.phase5_memory_score_final = 1.2, .phase5_thread_score_final = 1.0,
                            .phase5_epsilon = 0.1};
    passed = benefit_utility_action_margin_matches(&margins, VALIDATION_ACTION_MOVE_MEMORY) &&
             !benefit_utility_action_margin_matches(&margins, VALIDATION_ACTION_MOVE_THREAD) && passed;
    printf("BC20_SYMMETRIC_MARGIN_PREPARATION: %s\n", passed ? "PASS" : "FAIL");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
