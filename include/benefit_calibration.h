#ifndef AWAVMA_BENEFIT_CALIBRATION_H
#define AWAVMA_BENEFIT_CALIBRATION_H

#include "benefit_classifier.h"

#include <stddef.h>

#define BENEFIT_CALIBRATION_PROVENANCE_MAX 256U

typedef struct {
    BenefitCalibrationState state;
    char provenance[BENEFIT_CALIBRATION_PROVENANCE_MAX];
} BenefitCalibrationArtifact;

/* Loads a strict, topology-specific production calibration artifact. */
BenefitCalibrationState benefit_calibration_load(const char *path,
                                                 BenefitCalibrationArtifact *artifact);

#endif
