#ifndef AWAVMA_VALIDATION_H
#define AWAVMA_VALIDATION_H

#include "validation_types.h"

#include <stdbool.h>

typedef struct {
    double stability_weight;
    double sample_weight;
    double classifier_weight;
    double gain_weight;
    double cost_weight;
    double cpu_weight;
    double memory_weight;
    double concurrency_weight;
    double confidence_weight;
    double roi_weight;
    double safety_weight;
    double confidence_threshold;
    double roi_threshold;
    double safety_threshold;
    double validation_threshold;
    uint64_t sample_reference_count;
    double history_max_days;
    size_t history_max_records;
    size_t history_cleanup_interval;
    double history_decay_lambda;
    const char *results_path;
    const char *history_path;
    const char *log_path;
} ValidationConfig;

bool Validation_Init(const ValidationConfig *config);
ValidationResult ValidateMigration(const MonitorData *monitor,
                                   const DecisionData *decision,
                                   const ClassifierData *classifier);
GateResult EvaluateConfidenceGate(const MonitorData *monitor,
                                  const ClassifierData *classifier,
                                  const ValidationConfig *config);
GateResult EvaluateROIGate(const DecisionData *decision,
                           const ValidationConfig *config);
GateResult EvaluateSafetyGate(const MonitorData *monitor,
                              const DecisionData *decision,
                              const ValidationConfig *config);
ValidationResult CalculateValidationScore(const GateResult *confidence,
                                          const GateResult *roi,
                                          const GateResult *safety,
                                          const ValidationConfig *config);
bool LogValidationResult(const ValidationResult *result,
                         const DecisionData *decision);
void Validation_Shutdown(void);

#endif
