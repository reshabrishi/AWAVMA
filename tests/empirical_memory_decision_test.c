#include "empirical_memory_decision.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ValidatedCalibrationMatch match_for(void)
{
    ValidatedCalibrationMatch match = {0};
    match.status = CALIBRATION_MATCHED;
    match.calibration_status = CALIBRATION_VALIDATED_PRODUCTION;
    match.action = VALIDATION_ACTION_MOVE_MEMORY;
    match.source_node = 1; match.destination_node = 0; match.migration_page_bucket = 4096;
    match.expected_gain_pct = 8.0; match.effective_cost_pct = 7.0;
    snprintf(match.calibration_id, sizeof(match.calibration_id), "cal1-0123456789abcdef");
    snprintf(match.calibration_version, sizeof(match.calibration_version), "p4c-v2");
    snprintf(match.collection_experiment_id, sizeof(match.collection_experiment_id), "p4c-authoritative");
    return match;
}

static EmpiricalMemoryDecisionFacts facts_for(void)
{
    EmpiricalMemoryDecisionFacts facts = {0};
    facts.requested_action = VALIDATION_ACTION_MOVE_MEMORY;
    facts.identity_authoritative = facts.identity_current = true;
    facts.registration_authoritative = facts.registration_current = true;
    facts.activity_authoritative = facts.activity_current = true;
    facts.placement_authoritative = facts.memory_is_remote = facts.destination_is_local = true;
    facts.source_node = 1; facts.destination_node = 0; facts.migration_page_bucket = 4096;
    return facts;
}

int main(void)
{
    ValidatedCalibrationMatch match = match_for();
    EmpiricalMemoryDecisionFacts facts = facts_for();
    DecisionData decision;
    assert(empirical_memory_decision(&match, &facts, &decision));
    assert(decision.action == VALIDATION_ACTION_MOVE_MEMORY);
    assert(decision.evidence_model == DECISION_EVIDENCE_EMPIRICAL_GAIN_COST);
    assert(decision.predicted_gain == 8.0 && decision.estimated_cost == 7.0);
    assert(strcmp(decision.calibration_id, match.calibration_id) == 0);
    assert(strcmp(decision.calibration_provenance, "p4c-authoritative") == 0);
    facts.activity_authoritative = false;
    assert(!empirical_memory_decision(&match, &facts, &decision));
    assert(decision.action == VALIDATION_ACTION_NO_MIGRATION && !decision.gain_available);
    facts = facts_for(); facts.requested_action = VALIDATION_ACTION_MOVE_THREAD;
    assert(!empirical_memory_decision(&match, &facts, &decision));
    facts = facts_for(); facts.destination_node = 2;
    assert(!empirical_memory_decision(&match, &facts, &decision));
    facts = facts_for(); match.effective_cost_pct = match.expected_gain_pct;
    assert(!empirical_memory_decision(&match, &facts, &decision));
    assert(decision.action == VALIDATION_ACTION_NO_MIGRATION && decision.gain_available &&
           decision.cost_available && decision.empirical_roi_available && decision.empirical_roi == 0.0 &&
           strcmp(decision.status_text, "NOT_BENEFICIAL") == 0);
    match.effective_cost_pct = 9.0;
    assert(!empirical_memory_decision(&match, &facts, &decision));
    assert(decision.empirical_roi == -1.0 && decision.evidence_model == DECISION_EVIDENCE_EMPIRICAL_GAIN_COST);
    match = match_for(); match.calibration_status = CALIBRATION_VALIDATED_TEST_ONLY;
    assert(!empirical_memory_decision(&match, &facts, &decision));
    printf("empirical_memory_decision_test: PASS\n");
    return 0;
}
