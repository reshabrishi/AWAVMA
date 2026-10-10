#ifndef AWAVMA_EMPIRICAL_MEMORY_DECISION_H
#define AWAVMA_EMPIRICAL_MEMORY_DECISION_H

#include "calibration.h"

typedef struct {
    ValidationAction requested_action;
    bool identity_authoritative;
    bool identity_current;
    bool registration_authoritative;
    bool registration_current;
    bool activity_authoritative;
    bool activity_current;
    bool placement_authoritative;
    bool memory_is_remote;
    bool destination_is_local;
    int source_node;
    int destination_node;
    size_t migration_page_bucket;
} EmpiricalMemoryDecisionFacts;

/* Pure fail-closed decision: true means MOVE_MEMORY was empirically authorized. */
bool empirical_memory_decision(const ValidatedCalibrationMatch *match,
                               const EmpiricalMemoryDecisionFacts *facts,
                               DecisionData *decision);

#endif
