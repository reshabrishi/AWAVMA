#ifndef AWAVMA_BENEFIT_CALIBRATION_H
#define AWAVMA_BENEFIT_CALIBRATION_H

#include "benefit_classifier.h"

#include <stddef.h>

#define BENEFIT_CALIBRATION_PROVENANCE_MAX 256U

typedef struct {
    BenefitCalibrationState state;
    int source_node;
    int target_node;
    double throughput_gain_percent;
    double execution_time_improvement_percent;
    char provenance[BENEFIT_CALIBRATION_PROVENANCE_MAX];
} BenefitCalibrationArtifact;

/* Loads a strict, topology-specific production calibration artifact. */
BenefitCalibrationState benefit_calibration_load(const char *path,
                                                 BenefitCalibrationArtifact *artifact);

#endif
