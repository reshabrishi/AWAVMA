#include "benefit_classifier.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static BenefitClassification reject(BenefitDecision *decision, BenefitClassification classification,
                                    const char *reason, const char *detail)
{
    decision->classification = classification;
    snprintf(decision->reason, sizeof(decision->reason), "%s", reason);
    snprintf(decision->detail, sizeof(decision->detail), "%s", detail);
    return classification;
}

static bool utility_evidence_complete(const DecisionData *phase5)
{
    const double reconstructed_margin = phase5->phase5_memory_score_final -
                                        phase5->phase5_thread_score_final;

    if (phase5->evidence_model != DECISION_EVIDENCE_UTILITY_POLICY ||
        !phase5->phase5_evidence_available || !phase5->phase5_utility_available ||
        !phase5->phase5_decision_margin_available || !phase5->phase5_epsilon_available ||
        !phase5->phase5_versions_available ||
        strcmp(phase5->phase5_decision_status, "DECISION_VALID") != 0 ||
        !isfinite(phase5->phase5_memory_score_final) ||
        !isfinite(phase5->phase5_thread_score_final) ||
        !isfinite(phase5->phase5_decision_margin) || !isfinite(phase5->phase5_epsilon))
        return false;
    /* Serialization is decimal; this verifies recorded Phase 5 values without re-ranking. */
    return fabs(phase5->phase5_decision_margin - reconstructed_margin) <= 0.0000001;
}

bool benefit_utility_action_margin_matches(const DecisionData *decision,
                                           ValidationAction action)
{
    if (decision == NULL || !isfinite(decision->phase5_memory_score_final) ||
        !isfinite(decision->phase5_thread_score_final) || !isfinite(decision->phase5_epsilon))
        return false;
    if (action == VALIDATION_ACTION_MOVE_MEMORY)
        return decision->phase5_memory_score_final > decision->phase5_thread_score_final +
               decision->phase5_epsilon;
    if (action == VALIDATION_ACTION_MOVE_THREAD)
        return decision->phase5_thread_score_final > decision->phase5_memory_score_final +
               decision->phase5_epsilon;
    return false;
}

const char *benefit_classification_name(BenefitClassification classification)
{
    static const char *names[] = {"BENEFIT_SUPPORTED", "BENEFIT_NOT_SUPPORTED",
        "INSUFFICIENT_BENEFIT_EVIDENCE", "BENEFIT_POLICY_UNCALIBRATED",
        "STALE_OR_IDENTITY_MISMATCH", "ACTION_NOT_ELIGIBLE", "TARGET_NOT_VALIDATED",
        "PAGE_RECOVERY_REQUIRED"};

    return classification >= BENEFIT_SUPPORTED && classification <= PAGE_RECOVERY_REQUIRED ?
        names[classification] : "BENEFIT_CLASSIFICATION_ERROR";
}

const char *benefit_calibration_state_name(BenefitCalibrationState state)
{
    static const char *names[] = {"CALIBRATION_UNAVAILABLE", "CALIBRATION_CONFIGURED_UNVALIDATED",
        "CALIBRATION_VALIDATED_TEST_ONLY", "CALIBRATION_VALIDATED_PRODUCTION"};

    return state >= BENEFIT_CALIBRATION_UNAVAILABLE &&
        state <= BENEFIT_CALIBRATION_VALIDATED_PRODUCTION ? names[state] : "CALIBRATION_INVALID";
}

BenefitClassification benefit_classifier_evaluate(const BenefitClassifierInput *input,
                                                  BenefitDecision *decision)
{
    const DecisionData *phase5;
    const ValidationResult *phase6;
    const MigrationTarget *target;

    if (decision == NULL)
        return INSUFFICIENT_BENEFIT_EVIDENCE;
    memset(decision, 0, sizeof(*decision));
    decision->classification = INSUFFICIENT_BENEFIT_EVIDENCE;
    decision->source_node = -1;
    decision->target_node = -1;
    decision->confidence_status = GATE_INVALID;
    decision->roi_status = GATE_INVALID;
    decision->calibration_state = BENEFIT_CALIBRATION_UNAVAILABLE;
    if (input == NULL || input->attempt_id == NULL || input->attempt_id[0] == '\0')
        return reject(decision, INSUFFICIENT_BENEFIT_EVIDENCE, "BENEFIT_INPUT_UNAVAILABLE",
                      "benefit classifier input or attempt identity is unavailable");
    decision->pid = input->pid;
    decision->start_time_ticks = input->start_time_ticks;
    decision->action = input->action;
    decision->calibration_state = input->calibration.state;
    snprintf(decision->attempt_id, sizeof(decision->attempt_id), "%s", input->attempt_id);
    if (input->action != VALIDATION_ACTION_MOVE_THREAD &&
        input->action != VALIDATION_ACTION_MOVE_MEMORY)
        return reject(decision, ACTION_NOT_ELIGIBLE, "ACTION_NOT_ELIGIBLE",
                      "only an approved migration action is eligible");
    if (input->action == VALIDATION_ACTION_MOVE_MEMORY) {
        const MemoryRecoveryEvidence *recovery = input->memory_recovery;

        if (recovery == NULL || !recovery->checkpoint_complete ||
            !recovery->original_placement_known || !recovery->rollback_provider_retained ||
            recovery->pid != input->pid || recovery->start_time_ticks != input->start_time_ticks ||
            strcmp(recovery->attempt_id, input->attempt_id) != 0 ||
            recovery->candidate_count == 0 ||
            recovery->candidate_count != input->memory_candidate_count)
            return reject(decision, PAGE_RECOVERY_REQUIRED, "PAGE_RECOVERY_REQUIRED",
                          "complete attempt-bound page recovery evidence is required");
    }
    if (input->pid <= 0 || input->start_time_ticks == 0 || !input->process_active ||
        !input->identity_match)
        return reject(decision, STALE_OR_IDENTITY_MISMATCH, "IDENTITY_MISMATCH",
                      "candidate process identity is stale, inactive, or changed");
    phase5 = input->decision;
    phase6 = input->validation;
    target = input->target;
    if (!input->phase5_fresh || !input->phase6_fresh || !input->evidence_attempt_bound ||
        phase5 == NULL || phase6 == NULL ||
        phase5->pid != (long)input->pid || phase5->action != input->action ||
        phase6->pid != (long)input->pid || phase6->action != input->action)
        return reject(decision, STALE_OR_IDENTITY_MISMATCH, "STALE_PHASE5_PHASE6_RESULT",
                      "Phase 5/6 action evidence is missing, stale, or belongs to another candidate");
    if (target == NULL || !input->target_provider_validated || target->pid != input->pid ||
        target->start_time_ticks != input->start_time_ticks || target->action != input->action ||
        strcmp(target->attempt_id, input->attempt_id) != 0 || !target->source_node_known ||
        !target->has_target_numa_node ||
        (input->action == VALIDATION_ACTION_MOVE_THREAD && !target->has_target_cpu_mask)) {
        return reject(decision, TARGET_NOT_VALIDATED, "TARGET_METADATA_MISMATCH",
                      "target metadata is not structurally bound to this attempt");
    }
    decision->source_node = target->source_numa_node;
    decision->target_node = target->target_numa_node;
    decision->target_structurally_validated = true;
    if (input->action == VALIDATION_ACTION_MOVE_MEMORY &&
        (input->memory_recovery->source_numa_node != target->source_numa_node ||
         input->memory_recovery->target_numa_node != target->target_numa_node))
        return reject(decision, PAGE_RECOVERY_REQUIRED, "PAGE_RECOVERY_REQUIRED",
                      "recovery evidence NUMA placement does not match the migration target");
    if (!input->target_online || !input->target_permitted || !input->source_known ||
        !input->source_target_valid || target->source_numa_node == target->target_numa_node)
        return reject(decision, TARGET_NOT_VALIDATED, "TARGET_NOT_VALIDATED",
                      "source, target online state, or allowed-affinity relationship is invalid");
    if (input->quarantined)
        return reject(decision, BENEFIT_NOT_SUPPORTED, "QUARANTINED",
                      "application is quarantined");
    if (input->cooldown_active)
        return reject(decision, BENEFIT_NOT_SUPPORTED, "COOLDOWN_ACTIVE",
                      "application cooldown is active");
    if (input->history_suppressed)
        return reject(decision, BENEFIT_NOT_SUPPORTED, "HISTORY_SUPPRESSED",
                      "recent equivalent migration failure suppresses this target");
    if (strcmp(phase6->final_decision, "APPROVED") != 0)
        return reject(decision, BENEFIT_NOT_SUPPORTED, "PHASE6_NOT_APPROVED",
                      "Phase 6 did not approve this migration action");
    decision->confidence_status = phase6->confidence_status;
    decision->roi_status = phase6->roi_status;
    if (!isfinite(phase6->confidence_score) || !isfinite(phase6->safety_score) ||
        phase6->confidence_status == GATE_INVALID || phase6->safety_status == GATE_INVALID)
        return reject(decision, INSUFFICIENT_BENEFIT_EVIDENCE, "BENEFIT_EVIDENCE_UNAVAILABLE",
                      "required Phase 6 confidence or safety evidence is unavailable");
    if (phase6->confidence_status != GATE_PASS)
        return reject(decision, BENEFIT_NOT_SUPPORTED, "LOW_CONFIDENCE",
                      "existing Phase 6 confidence gate did not pass");
    /* Empirical memory safety is evaluated by the attempt-bound safety manager below. */
    if (phase6->safety_status != GATE_PASS &&
        !(phase5->evidence_model == DECISION_EVIDENCE_EMPIRICAL_GAIN_COST &&
          phase6->safety_status == GATE_NOT_APPLICABLE))
        return reject(decision, BENEFIT_NOT_SUPPORTED, "UNSAFE",
                      "existing Phase 6 safety gate did not pass");
    if (phase5->evidence_model == DECISION_EVIDENCE_UTILITY_POLICY) {
        if (!utility_evidence_complete(phase5))
            return reject(decision, INSUFFICIENT_BENEFIT_EVIDENCE, "UTILITY_EVIDENCE_UNAVAILABLE",
                          "canonical Phase 5 utility evidence is incomplete or inconsistent");
        if (!benefit_utility_action_margin_matches(phase5, input->action))
            return reject(decision, BENEFIT_NOT_SUPPORTED, "PHASE5_EPSILON_ACTION_GATE_NOT_PASSED",
                           "recorded Phase 5 utility margin does not authorize the action");
        if (phase6->roi_status != GATE_NOT_APPLICABLE)
            return reject(decision, INSUFFICIENT_BENEFIT_EVIDENCE,
                          "ROI_UTILITY_MODEL_CONTRACT_MISMATCH",
                          "utility-policy evidence requires ROI to be recorded as not applicable");
    } else if (phase5->evidence_model == DECISION_EVIDENCE_EMPIRICAL_GAIN_COST) {
        if (!phase5->gain_available || !phase5->cost_available || !isfinite(phase5->predicted_gain) ||
            !isfinite(phase5->estimated_cost) || !isfinite(phase6->roi_score) ||
            phase6->roi_status == GATE_INVALID)
            return reject(decision, INSUFFICIENT_BENEFIT_EVIDENCE, "BENEFIT_EVIDENCE_UNAVAILABLE",
                          "required empirical gain/cost or Phase 6 ROI evidence is unavailable");
        if (phase6->roi_status != GATE_PASS)
            return reject(decision, BENEFIT_NOT_SUPPORTED, "INSUFFICIENT_ROI",
                          "existing Phase 6 ROI gate did not pass");
    } else {
        return reject(decision, INSUFFICIENT_BENEFIT_EVIDENCE, "BENEFIT_EVIDENCE_MODEL_UNAVAILABLE",
                      "Phase 5 evidence model is unavailable");
    }
    if (input->calibration.state != BENEFIT_CALIBRATION_VALIDATED_PRODUCTION &&
        input->calibration.state != BENEFIT_CALIBRATION_VALIDATED_TEST_ONLY)
        return reject(decision, BENEFIT_POLICY_UNCALIBRATED, "BENEFIT_POLICY_UNCALIBRATED",
                      "no validated cross-NUMA production benefit calibration is available");
    return reject(decision, BENEFIT_SUPPORTED, "BENEFIT_SUPPORTED",
                  input->calibration.state == BENEFIT_CALIBRATION_VALIDATED_TEST_ONLY ?
                  "synthetic test-only calibration accepted; not production benefit evidence" :
                  "validated production calibration and existing Phase 5/6 evidence passed");
}
