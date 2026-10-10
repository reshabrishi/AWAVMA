#include "benefit_classifier.h"

#include <math.h>
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
    phase5.evidence_model = DECISION_EVIDENCE_UTILITY_POLICY;
    phase5.phase5_evidence_available = true;
    phase5.phase5_utility_available = true;
    phase5.phase5_decision_margin_available = true;
    phase5.phase5_epsilon_available = true;
    phase5.phase5_versions_available = true;
    phase5.phase5_memory_score_raw = 0.0;
    phase5.phase5_thread_score_raw = 0.24;
    phase5.phase5_memory_score_final = 0.0;
    phase5.phase5_thread_score_final = 0.24;
    phase5.phase5_decision_margin = -0.24;
    phase5.phase5_epsilon = 0.05;
    phase5.phase5_weight_version = 1;
    phase5.phase5_bias_version = 1;
    snprintf(phase5.migration_id, sizeof(phase5.migration_id), "m_42_1_0");
    snprintf(phase5.phase5_decision_status, sizeof(phase5.phase5_decision_status), "DECISION_VALID");
    phase6.pid = 42;
    phase6.action = VALIDATION_ACTION_MOVE_THREAD;
    phase6.confidence_status = GATE_PASS;
    phase6.safety_status = GATE_PASS;
    phase6.roi_status = GATE_NOT_APPLICABLE;
    phase6.confidence_score = 80.0;
    phase6.safety_score = 80.0;
    phase6.roi_score = -1.0;
    snprintf(phase6.migration_id, sizeof(phase6.migration_id), "m_42_1_0");
    snprintf(phase6.final_decision, sizeof(phase6.final_decision), "APPROVED");
    target.pid = 42;
    target.start_time_ticks = 99;
    target.action = VALIDATION_ACTION_MOVE_THREAD;
    target.source_node_known = true;
    target.source_numa_node = 0;
    target.has_target_numa_node = true;
    target.target_numa_node = 1;
    target.has_target_cpu_mask = true;
    CPU_SET(2, &target.target_cpu_mask);
    snprintf(target.attempt_id, sizeof(target.attempt_id), "attempt-1");
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
    input.calibration.provenance = "strict-test-only-calibration";
    return input;
}

static bool expect(const char *name, BenefitClassifierInput *input, BenefitClassification expected)
{
    BenefitDecision decision;
    BenefitClassification actual = benefit_classifier_evaluate(input, &decision);
    bool passed = actual == expected;

    printf("%s: %s\n", name, passed ? "PASS" : "FAIL");
    return passed;
}

int main(void)
{
    BenefitClassifierInput input;
    MemoryRecoveryEvidence recovery;
    bool passed = true;

    input = input_for();
    passed = expect("BEC01_COMPLETE_UTILITY_EVIDENCE_SUPPORTED_TEST_ONLY", &input, BENEFIT_SUPPORTED) && passed;
    input = input_for();
    passed = expect("BEC02_NO_GAIN_COST_REQUIRED_FOR_UTILITY_MODEL", &input, BENEFIT_SUPPORTED) && passed;
    input = input_for(); input.calibration.state = BENEFIT_CALIBRATION_UNAVAILABLE;
    passed = expect("BEC03_UTILITY_EVIDENCE_UNCALIBRATED", &input, BENEFIT_POLICY_UNCALIBRATED) && passed;
    input = input_for(); ((DecisionData *)input.decision)->phase5_utility_available = false;
    passed = expect("BEC04_SELECTED_OR_ALTERNATIVE_UTILITY_REQUIRED", &input, INSUFFICIENT_BENEFIT_EVIDENCE) && passed;
    input = input_for(); ((DecisionData *)input.decision)->phase5_thread_score_final = NAN;
    passed = expect("BEC05_FINITE_ALTERNATIVE_UTILITY_REQUIRED", &input, INSUFFICIENT_BENEFIT_EVIDENCE) && passed;
    input = input_for(); ((DecisionData *)input.decision)->phase5_decision_margin_available = false;
    passed = expect("BEC06_MARGIN_REQUIRED", &input, INSUFFICIENT_BENEFIT_EVIDENCE) && passed;
    input = input_for();
    ((DecisionData *)input.decision)->phase5_memory_score_final = 0.0;
    ((DecisionData *)input.decision)->phase5_thread_score_final = 0.0;
    ((DecisionData *)input.decision)->phase5_decision_margin = 0.0;
    passed = expect("BEC07_ZERO_MARGIN_NOT_UNAVAILABLE", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); ((DecisionData *)input.decision)->phase5_epsilon_available = false;
    passed = expect("BEC08_EPSILON_REQUIRED", &input, INSUFFICIENT_BENEFIT_EVIDENCE) && passed;
    input = input_for(); input.phase5_fresh = false;
    passed = expect("BEC09_STALE_GENERATION_REJECTED", &input, STALE_OR_IDENTITY_MISMATCH) && passed;
    input = input_for(); input.phase6_fresh = false;
    passed = expect("BEC10_MIGRATION_ID_MISMATCH_REJECTED", &input, STALE_OR_IDENTITY_MISMATCH) && passed;
    input = input_for(); input.evidence_attempt_bound = false;
    passed = expect("BEC11_ATTEMPT_BINDING_REQUIRED", &input, STALE_OR_IDENTITY_MISMATCH) && passed;
    input = input_for(); ((ValidationResult *)input.validation)->roi_status = GATE_PASS;
    passed = expect("BEC12_ROI_MUST_BE_NOT_APPLICABLE", &input, INSUFFICIENT_BENEFIT_EVIDENCE) && passed;
    input = input_for();
    ((DecisionData *)input.decision)->phase5_thread_score_final = 0.01;
    ((DecisionData *)input.decision)->phase5_decision_margin = -0.01;
    passed = expect("BEC13_EPSILON_ACTION_GATE_REQUIRED", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); input.cooldown_active = true;
    passed = expect("BEC14_COOLDOWN_RESPECTED", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); input.quarantined = true;
    passed = expect("BEC15_QUARANTINE_RESPECTED", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); input.history_suppressed = true;
    passed = expect("BEC16_HISTORY_SUPPRESSION_RESPECTED", &input, BENEFIT_NOT_SUPPORTED) && passed;
    input = input_for(); input.target_provider_validated = false;
    passed = expect("BEC17_INVALID_TARGET_REJECTED", &input, TARGET_NOT_VALIDATED) && passed;
    input = input_for(); input.action = VALIDATION_ACTION_MOVE_MEMORY;
    passed = expect("BEC18_PAGE_RECOVERY_REQUIRED", &input, PAGE_RECOVERY_REQUIRED) && passed;
    input = input_for();
    memset(&recovery, 0, sizeof(recovery));
    input.action = VALIDATION_ACTION_MOVE_MEMORY;
    ((DecisionData *)input.decision)->action = VALIDATION_ACTION_MOVE_MEMORY;
    ((DecisionData *)input.decision)->phase5_memory_score_final = 0.24;
    ((DecisionData *)input.decision)->phase5_thread_score_final = 0.0;
    ((DecisionData *)input.decision)->phase5_decision_margin = 0.24;
    ((ValidationResult *)input.validation)->action = VALIDATION_ACTION_MOVE_MEMORY;
    ((MigrationTarget *)input.target)->action = VALIDATION_ACTION_MOVE_MEMORY;
    ((MigrationTarget *)input.target)->has_target_cpu_mask = false;
    recovery.pid = input.pid;
    recovery.start_time_ticks = input.start_time_ticks;
    snprintf(recovery.attempt_id, sizeof(recovery.attempt_id), "%s", input.attempt_id);
    recovery.candidate_count = 2;
    recovery.checkpoint_complete = true;
    recovery.original_placement_known = true;
    recovery.rollback_provider_retained = true;
    recovery.source_numa_node = 0;
    recovery.target_numa_node = 1;
    input.memory_recovery = &recovery;
    input.memory_candidate_count = 2;
    passed = expect("BEC19_COMPLETE_MEMORY_RECOVERY_SUPPORTED", &input, BENEFIT_SUPPORTED) && passed;
    recovery.candidate_count = 1;
    passed = expect("BEC20_MEMORY_RECOVERY_COUNT_MISMATCH", &input, PAGE_RECOVERY_REQUIRED) && passed;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
