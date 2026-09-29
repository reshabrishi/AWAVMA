#ifndef AWAVMA_BENEFIT_CLASSIFIER_H
#define AWAVMA_BENEFIT_CLASSIFIER_H

#include "migration_target_provider.h"

#include <stdbool.h>

typedef enum {
    BENEFIT_SUPPORTED,
    BENEFIT_NOT_SUPPORTED,
    INSUFFICIENT_BENEFIT_EVIDENCE,
    BENEFIT_POLICY_UNCALIBRATED,
    STALE_OR_IDENTITY_MISMATCH,
    ACTION_NOT_ELIGIBLE,
    TARGET_NOT_VALIDATED,
    PAGE_RECOVERY_REQUIRED
} BenefitClassification;

typedef enum {
    BENEFIT_CALIBRATION_UNAVAILABLE,
    BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED,
    BENEFIT_CALIBRATION_VALIDATED_TEST_ONLY,
    BENEFIT_CALIBRATION_VALIDATED_PRODUCTION
} BenefitCalibrationState;

/* Calibration is provenance, not an additional benefit scoring policy. */
typedef struct {
    BenefitCalibrationState state;
    const char *provenance;
} BenefitCalibration;

typedef struct {
    pid_t pid;
    uint64_t start_time_ticks;
    const char *attempt_id;
    ValidationAction action;
    bool process_active;
    bool identity_match;
    bool phase5_fresh;
    bool phase6_fresh;
    bool evidence_attempt_bound;
    const DecisionData *decision;
    const ValidationResult *validation;
    const MigrationTarget *target;
    bool target_provider_validated;
    bool target_online;
    bool target_permitted;
    bool source_known;
    bool source_target_valid;
    bool cooldown_active;
    bool quarantined;
    bool history_suppressed;
    BenefitCalibration calibration;
} BenefitClassifierInput;

typedef struct {
    BenefitClassification classification;
    pid_t pid;
    uint64_t start_time_ticks;
    char attempt_id[128];
    ValidationAction action;
    int source_node;
    int target_node;
    bool target_structurally_validated;
    GateStatus confidence_status;
    GateStatus roi_status;
    BenefitCalibrationState calibration_state;
    char reason[128];
    char detail[192];
} BenefitDecision;

const char *benefit_classification_name(BenefitClassification classification);
const char *benefit_calibration_state_name(BenefitCalibrationState state);
BenefitClassification benefit_classifier_evaluate(const BenefitClassifierInput *input,
                                                  BenefitDecision *decision);

#endif
