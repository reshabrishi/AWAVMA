#include "empirical_memory_decision.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool empirical_memory_decision(const ValidatedCalibrationMatch *match,
                               const EmpiricalMemoryDecisionFacts *facts,
                               DecisionData *decision)
{
    if (decision == NULL)
        return false;
    memset(decision, 0, sizeof(*decision));
    decision->action = VALIDATION_ACTION_NO_MIGRATION;
    snprintf(decision->action_text, sizeof(decision->action_text), "NO_MIGRATION");
    snprintf(decision->status_text, sizeof(decision->status_text), "EMPIRICAL_NOT_AUTHORIZED");

    if (match == NULL || facts == NULL || match->status != CALIBRATION_MATCHED ||
        match->calibration_status != CALIBRATION_VALIDATED_PRODUCTION ||
        strcmp(match->calibration_version, "p4c-v1") != 0 ||
        match->action != VALIDATION_ACTION_MOVE_MEMORY ||
        facts->requested_action != VALIDATION_ACTION_MOVE_MEMORY ||
        !facts->identity_authoritative || !facts->identity_current ||
        !facts->registration_authoritative || !facts->registration_current ||
        !facts->activity_authoritative || !facts->activity_current ||
        !facts->placement_authoritative || !facts->memory_is_remote ||
        !facts->destination_is_local || facts->source_node != match->source_node ||
        facts->destination_node != match->destination_node ||
        facts->migration_page_bucket != match->migration_page_bucket ||
        !isfinite(match->expected_gain_pct) || !isfinite(match->effective_cost_pct))
        return false;

    decision->source_node = facts->source_node;
    decision->destination_node = facts->destination_node;
    decision->nodes_available = true;
    decision->predicted_gain = match->expected_gain_pct;
    decision->estimated_cost = match->effective_cost_pct;
    decision->gain_available = true;
    decision->cost_available = true;
    decision->empirical_roi = match->expected_gain_pct - match->effective_cost_pct;
    decision->empirical_roi_available = true;
    decision->evidence_model = DECISION_EVIDENCE_EMPIRICAL_GAIN_COST;
    snprintf(decision->calibration_id, sizeof(decision->calibration_id), "%s", match->calibration_id);
    snprintf(decision->calibration_version, sizeof(decision->calibration_version), "%s", match->calibration_version);
    snprintf(decision->calibration_provenance, sizeof(decision->calibration_provenance), "%s", match->collection_experiment_id);
    if (decision->empirical_roi <= 0.0) {
        snprintf(decision->status_text, sizeof(decision->status_text), "NOT_BENEFICIAL");
        return false;
    }

    decision->action = VALIDATION_ACTION_MOVE_MEMORY;
    snprintf(decision->action_text, sizeof(decision->action_text), "MOVE_MEMORY");
    snprintf(decision->status_text, sizeof(decision->status_text), "EMPIRICAL_AUTHORIZED");
    return true;
}
