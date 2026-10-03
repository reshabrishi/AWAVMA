#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool write_fixture_rows(const char *path, const char *header, const char *row)
{
    FILE *file = fopen(path, "w");
    bool written;

    if (file == NULL)
        return false;
    written = fputs(header, file) >= 0 && fputs(row, file) >= 0;
    return fclose(file) == 0 && written;
}

static bool equal(double left, double right)
{
    return fabs(left - right) < 0.0000001;
}

int main(void)
{
    char directory[] = "/tmp/phase5-benefit-evidence-XXXXXX";
    char input_path[256];
    char validation_path[256];
    awavma_runtime_record_t record = {.pid = 42, .generation = 17};
    DecisionData decision;
    ValidationResult validation;
    bool passed;

    if (mkdtemp(directory) == NULL)
        return EXIT_FAILURE;
    snprintf(input_path, sizeof(input_path), "%s/validation_input.csv", directory);
    snprintf(validation_path, sizeof(validation_path), "%s/validation.csv", directory);
    snprintf(record.app_id, sizeof(record.app_id), "evidence-app");
    const char *input_header =
        "timestamp,migration_id,app_id,pid,entity_id,action,decision_status,classification,predicted_gain,estimated_cost,evidence_model,phase5_timestamp,phase5_runtime_generation,phase5_evidence_provenance,phase5_classification_score,phase5_f_access,phase5_f_threshold,phase5_f_gain_memory,phase5_f_cost_memory,phase5_f_cpu_memory,phase5_f_sharing_memory,phase5_f_gain_thread,phase5_f_cost_thread,phase5_f_cpu_thread,phase5_f_sharing_thread,phase5_memory_score_raw,phase5_thread_score_raw,phase5_memory_bias,phase5_thread_bias,phase5_memory_score_final,phase5_thread_score_final,phase5_decision_margin,phase5_epsilon,phase5_weight_version,phase5_bias_version,candidate_tid,candidate_pid\n";
    const char *input_row =
        "2026-09-07T12:00:00,m_42_17_0,evidence-app,42,314,MOVE_THREAD,DECISION_VALID,HOT,84.5,21.25,EMPIRICAL_GAIN_COST_EVIDENCE,2026-09-07T12:00:00,17,PHASE5_DECISION_ENGINE,91.25,0.10,0.20,0.30,0.40,0.50,0.60,0.70,0.80,0.90,1.00,1.10,2.20,0.03,0.04,1.13,2.24,-1.11,0.05,7,3,314,42\n";
    const char *validation_header =
        "timestamp,migration_id,app_id,pid,entity_id,action,source_node,destination_node,confidence_score,roi_score,safety_score,validation_score,confidence_status,roi_status,safety_status,validation_status,final_decision\n";
    const char *validation_row =
        "2026-09-07T12:00:01,m_42_17_0,evidence-app,42,314,MOVE_THREAD,0,1,82.5,63.25,71.0,72.5,PASS,PASS,PASS,PASS,APPROVED\n";
    passed = write_fixture_rows(input_path, input_header, input_row) &&
        write_fixture_rows(validation_path, validation_header, validation_row) &&
        awavma_runtime_test_load_benefit_evidence(input_path, validation_path, &record, &decision,
                                                  &validation) == 1 &&
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
        validation.confidence_status == GATE_PASS && validation.roi_status == GATE_PASS &&
        equal(validation.confidence_score, 82.5) && equal(validation.roi_score, 63.25) &&
        strcmp(validation.migration_id, decision.migration_id) == 0;
    printf("P5BE01_EMPIRICAL_MOVE_THREAD_APPROVED_JOIN: %s\n", passed ? "PASS" : "FAIL");
    awavma_runtime_record_t stale_record = record;

    stale_record.generation++;
    bool stale_rejected = awavma_runtime_test_load_benefit_evidence(input_path, validation_path,
                                                                     &stale_record, &decision,
                                                                     &validation) != 1;
    printf("P5BE02_STALE_GENERATION_REJECTED: %s\n", stale_rejected ? "PASS" : "FAIL");
    bool malformed_ids_rejected =
        write_fixture_rows(validation_path, validation_header,
            "2026-09-07T12:00:01,m_42_17_0,evidence-app,42oops,314,MOVE_THREAD,0,1,82.5,63.25,71.0,72.5,PASS,PASS,PASS,PASS,APPROVED\n") &&
        awavma_runtime_test_load_benefit_evidence(input_path, validation_path, &record, &decision,
                                                  &validation) != 1 &&
        write_fixture_rows(validation_path, validation_header, validation_row) &&
        write_fixture_rows(input_path, input_header,
            "2026-09-07T12:00:00,m_42_17_0,evidence-app,42oops,314,MOVE_THREAD,DECISION_VALID,HOT,84.5,21.25,EMPIRICAL_GAIN_COST_EVIDENCE,2026-09-07T12:00:00,17,PHASE5_DECISION_ENGINE,91.25,0.10,0.20,0.30,0.40,0.50,0.60,0.70,0.80,0.90,1.00,1.10,2.20,0.03,0.04,1.13,2.24,-1.11,0.05,7,3,314,42\n") &&
        awavma_runtime_test_load_benefit_evidence(input_path, validation_path, &record, &decision,
                                                  &validation) != 1 &&
        write_fixture_rows(input_path, input_header,
            "2026-09-07T12:00:00,m_42_17_0,evidence-app,42,314oops,MOVE_THREAD,DECISION_VALID,HOT,84.5,21.25,EMPIRICAL_GAIN_COST_EVIDENCE,2026-09-07T12:00:00,17,PHASE5_DECISION_ENGINE,91.25,0.10,0.20,0.30,0.40,0.50,0.60,0.70,0.80,0.90,1.00,1.10,2.20,0.03,0.04,1.13,2.24,-1.11,0.05,7,3,314oops,42\n") &&
        awavma_runtime_test_load_benefit_evidence(input_path, validation_path, &record, &decision,
                                                  &validation) != 1;
    printf("P5BE03_MALFORMED_PID_AND_TID_REJECTED: %s\n", malformed_ids_rejected ? "PASS" : "FAIL");
    bool identity_join_rejected =
        write_fixture_rows(input_path, input_header,
            "2026-09-07T12:00:00,m_42_17_0,other-app,42,314,MOVE_THREAD,DECISION_VALID,HOT,84.5,21.25,EMPIRICAL_GAIN_COST_EVIDENCE,2026-09-07T12:00:00,17,PHASE5_DECISION_ENGINE,91.25,0.10,0.20,0.30,0.40,0.50,0.60,0.70,0.80,0.90,1.00,1.10,2.20,0.03,0.04,1.13,2.24,-1.11,0.05,7,3,314,42\n") &&
        awavma_runtime_test_load_benefit_evidence(input_path, validation_path, &record, &decision,
                                                  &validation) != 1;
    printf("P5BE04_PHASE5_PHASE6_IDENTITY_JOIN_REJECTED: %s\n", identity_join_rejected ? "PASS" : "FAIL");
    passed = passed && stale_rejected && malformed_ids_rejected && identity_join_rejected;
    unlink(input_path);
    unlink(validation_path);
    rmdir(directory);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
