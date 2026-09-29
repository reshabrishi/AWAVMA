#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool write_fixture(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");

    if (file == NULL)
        return false;
    fputs(contents, file);
    return fclose(file) == 0;
}

static bool equal(double left, double right)
{
    return fabs(left - right) < 0.0000001;
}

int main(void)
{
    char directory[] = "/tmp/phase5-benefit-evidence-XXXXXX";
    char decision_path[256];
    char input_path[256];
    char validation_path[256];
    awavma_runtime_record_t record = {.pid = 42, .generation = 17};
    DecisionData decision;
    ValidationResult validation;
    MigrationTarget target = {0};
    BenefitClassifierInput classifier_input = {0};
    BenefitDecision classifier_decision;
    bool passed;

    if (mkdtemp(directory) == NULL)
        return EXIT_FAILURE;
    snprintf(decision_path, sizeof(decision_path), "%s/decision.csv", directory);
    snprintf(input_path, sizeof(input_path), "%s/validation_input.csv", directory);
    snprintf(validation_path, sizeof(validation_path), "%s/validation.csv", directory);
    snprintf(record.app_id, sizeof(record.app_id), "evidence-app");
    passed = write_fixture(decision_path,
        "timestamp,app_id,pid,entity_id,classification,classification_score,f_access,f_threshold,f_gain_memory,f_cost_memory,f_cpu_memory,f_sharing_memory,f_gain_thread,f_cost_thread,f_cpu_thread,f_sharing_thread,memory_score_raw,thread_score_raw,memory_bias,thread_bias,memory_score_final,thread_score_final,decision_margin,epsilon,decision,weight_version,bias_version,status,predicted_gain,estimated_cost\n"
        "2026-09-07T12:00:00,evidence-app,42,worker-1,HOT,91.25,0.10,0.20,0.30,0.40,0.50,0.60,0.70,0.80,0.90,1.00,1.10,2.20,0.03,0.04,1.13,2.24,-1.11,0.05,MOVE_THREAD,7,3,DECISION_VALID,84.5,21.25\n") &&
        awavma_runtime_test_write_validation_input(decision_path, input_path, &record) == 0 &&
        write_fixture(validation_path,
        "timestamp,migration_id,app_id,pid,entity_id,action,source_node,destination_node,confidence_score,roi_score,safety_score,validation_score,confidence_status,roi_status,safety_status,validation_status,final_decision\n"
        "2026-09-07T12:00:01,m_42_17_0,evidence-app,42,worker-1,MOVE_THREAD,0,1,82.5,-1,71.0,-1,PASS,NOT_APPLICABLE,PASS,PASS_ROI_NOT_APPLICABLE_TO_UTILITY_MODEL,APPROVED\n") &&
        awavma_runtime_test_load_benefit_evidence(input_path, validation_path, &record,
                                                  &decision, &validation) == 1 &&
        decision.phase5_evidence_available && decision.phase5_runtime_generation == 17 &&
        decision.phase5_utility_available && decision.phase5_decision_margin_available &&
        decision.phase5_epsilon_available &&
        decision.phase5_versions_available && decision.gain_available && decision.cost_available &&
        equal(decision.phase5_classification_score, 91.25) &&
        equal(decision.phase5_factors[6], 0.70) && equal(decision.phase5_memory_score_final, 1.13) &&
        equal(decision.phase5_thread_score_final, 2.24) &&
        equal(decision.phase5_decision_margin, -1.11) && equal(decision.phase5_epsilon, 0.05) &&
        decision.phase5_weight_version == 7 && decision.phase5_bias_version == 3 &&
        equal(decision.predicted_gain, 84.5) && equal(decision.estimated_cost, 21.25) &&
        validation.action == VALIDATION_ACTION_MOVE_THREAD &&
        validation.confidence_status == GATE_PASS && validation.roi_status == GATE_NOT_APPLICABLE &&
        equal(validation.confidence_score, 82.5) && equal(validation.roi_score, -1.0) &&
        strcmp(validation.migration_id, decision.migration_id) == 0;
    printf("P5BE01_ADAPTER_AND_JOIN_PRESERVE_EVIDENCE: %s\n", passed ? "PASS" : "FAIL");
    awavma_runtime_record_t stale_record = record;

    stale_record.generation++;
    bool stale_rejected = awavma_runtime_test_load_benefit_evidence(input_path, validation_path,
                                                                     &stale_record, &decision,
                                                                     &validation) != 1;
    printf("P5BE02_STALE_GENERATION_REJECTED: %s\n", stale_rejected ? "PASS" : "FAIL");
    memset(&target, 0, sizeof(target));
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
    classifier_input.pid = 42;
    classifier_input.start_time_ticks = 99;
    classifier_input.attempt_id = "attempt-1";
    classifier_input.action = VALIDATION_ACTION_MOVE_THREAD;
    classifier_input.process_active = true;
    classifier_input.identity_match = true;
    classifier_input.phase5_fresh = true;
    classifier_input.phase6_fresh = true;
    classifier_input.evidence_attempt_bound = true;
    classifier_input.decision = &decision;
    classifier_input.validation = &validation;
    classifier_input.target = &target;
    classifier_input.target_provider_validated = true;
    classifier_input.target_online = true;
    classifier_input.target_permitted = true;
    classifier_input.source_known = true;
    classifier_input.source_target_valid = true;
    classifier_input.calibration.state = BENEFIT_CALIBRATION_UNAVAILABLE;
    bool uncalibrated = benefit_classifier_evaluate(&classifier_input, &classifier_decision) ==
                        BENEFIT_POLICY_UNCALIBRATED;
    printf("P5BE03_COMPLETE_EVIDENCE_UNCALIBRATED: %s\n", uncalibrated ? "PASS" : "FAIL");
    passed = passed && stale_rejected && uncalibrated;
    unlink(decision_path);
    unlink(input_path);
    unlink(validation_path);
    rmdir(directory);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
