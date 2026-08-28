#ifndef AWAVMA_FEEDBACK_TYPES_H
#define AWAVMA_FEEDBACK_TYPES_H

#include "decision.h"
#include "validation_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEEDBACK_METRIC_COUNT 4U
#define FEEDBACK_SIGNAL_COUNT (DECISION_FACTOR_COUNT * 2U)

typedef enum {
    FEEDBACK_METRIC_THROUGHPUT,
    FEEDBACK_METRIC_EXECUTION_TIME,
    FEEDBACK_METRIC_LATENCY,
    FEEDBACK_METRIC_PAGE_FAULTS
} FeedbackMetric;

typedef enum {
    FEEDBACK_STRONGLY_POSITIVE,
    FEEDBACK_POSITIVE,
    FEEDBACK_NEUTRAL,
    FEEDBACK_NEGATIVE,
    FEEDBACK_STRONGLY_NEGATIVE,
    FEEDBACK_NOT_LEARNABLE,
    FEEDBACK_INSUFFICIENT_OBSERVATION,
    FEEDBACK_INVALID_INPUT,
    FEEDBACK_INVALID_STATE,
    FEEDBACK_NO_UPDATE
} FeedbackClass;

typedef enum {
    FEEDBACK_UPDATED,
    FEEDBACK_UPDATE_NOT_LEARNABLE,
    FEEDBACK_UPDATE_INSUFFICIENT_OBSERVATION,
    FEEDBACK_UPDATE_NO_UPDATE,
    FEEDBACK_UPDATE_INVALID_STATE,
    FEEDBACK_UPDATE_INVALID_INPUT
} FeedbackUpdateStatus;

typedef struct {
    char timestamp[32];
    char feedback_id[128];
    char migration_id[128];
    char app_id[128];
    long pid;
    char entity_id[128];
    ValidationAction action;
    char phase6_validation[64];
    char migration_result[64];
    long long recorded_at_epoch;
    bool epoch_available;
    double before_metrics[FEEDBACK_METRIC_COUNT];
    double after_metrics[FEEDBACK_METRIC_COUNT];
    bool metric_available[FEEDBACK_METRIC_COUNT];
    size_t before_samples;
    size_t after_samples;
    bool sample_counts_available;
    double supplied_confidence;
    bool supplied_confidence_available;
    double factor_signals[FEEDBACK_SIGNAL_COUNT];
    bool factor_available[FEEDBACK_SIGNAL_COUNT];
    double migration_execution_time_ms;
    bool migration_cost_available;
    size_t pages_migrated;
    size_t pages_failed;
    bool verification_available;
    bool verification_succeeded;
} FeedbackEvent;

typedef struct {
    FeedbackClass feedback_class;
    FeedbackUpdateStatus update_status;
    double raw_reward;
    double effective_reward;
    double feedback_confidence;
    double historical_relevance;
    size_t metrics_used;
    char reason[160];
    decision_feedback_snapshot_t state;
    decision_feedback_update_t applied_update;
    double processing_time_ms;
} FeedbackResult;

#endif
