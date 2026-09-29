#ifndef AWAVMA_FEEDBACK_TYPES_H
#define AWAVMA_FEEDBACK_TYPES_H

#include "decision.h"
#include "benefit_classifier.h"
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

/* Terminal safety events are persisted by Phase 8 but never train Phase 5. */
typedef enum {
    FEEDBACK_EVENT_MIGRATION_OBSERVATION,
    FEEDBACK_EVENT_MIGRATION_TERMINAL
} FeedbackEventKind;

typedef enum {
    FEEDBACK_TERMINAL_COMMITTED,
    FEEDBACK_TERMINAL_REJECTED,
    FEEDBACK_TERMINAL_EXECUTION_FAILED,
    FEEDBACK_TERMINAL_TIMEOUT,
    FEEDBACK_TERMINAL_DEGRADED,
    FEEDBACK_TERMINAL_SUSPECTED_STALL,
    FEEDBACK_TERMINAL_ROLLBACK_SUCCEEDED,
    FEEDBACK_TERMINAL_ROLLBACK_FAILED,
    FEEDBACK_TERMINAL_COOLDOWN,
    FEEDBACK_TERMINAL_QUARANTINED,
    FEEDBACK_TERMINAL_TARGET_GONE,
    FEEDBACK_TERMINAL_IDENTITY_CHANGED,
    FEEDBACK_TERMINAL_SUPPRESSED,
    FEEDBACK_TERMINAL_VALIDATION_UNKNOWN,
    FEEDBACK_TERMINAL_EXECUTION_DISABLED,
    FEEDBACK_TERMINAL_TARGET_UNAVAILABLE,
    FEEDBACK_TERMINAL_NO_ALTERNATE_TARGET,
    FEEDBACK_TERMINAL_TARGET_INVALID,
    FEEDBACK_TERMINAL_TARGET_STALE,
    FEEDBACK_TERMINAL_TARGET_IDENTITY_MISMATCH,
    FEEDBACK_TERMINAL_TARGET_TOPOLOGY_UNAVAILABLE
} FeedbackTerminalOutcome;

typedef enum {
    FEEDBACK_UPDATED,
    FEEDBACK_UPDATE_NOT_LEARNABLE,
    FEEDBACK_UPDATE_INSUFFICIENT_OBSERVATION,
    FEEDBACK_UPDATE_NO_UPDATE,
    FEEDBACK_UPDATE_INVALID_STATE,
    FEEDBACK_UPDATE_INVALID_INPUT,
    FEEDBACK_UPDATE_TERMINAL_RECORDED
} FeedbackUpdateStatus;

typedef struct {
    char timestamp[32];
    char feedback_id[128];
    char migration_id[128];
    char app_id[128];
    long pid;
    uint64_t start_time_ticks;
    bool start_time_ticks_available;
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
    FeedbackEventKind event_kind;
    FeedbackTerminalOutcome terminal_outcome;
    char terminal_reason[128];
    char target_selection_reason[256];
    bool structural_validation_known;
    bool structural_validation_succeeded;
    bool progress_known;
    bool progress_observed;
    bool benefit_known;
    bool benefit_classification_available;
    BenefitClassification benefit_classification;
    char benefit_reason[128];
    /* Provenance only; terminal feedback does not recalculate benefit evidence. */
    char phase5_evidence_provenance[48];
    uint64_t phase5_runtime_generation;
    bool phase5_runtime_generation_available;
    char phase5_migration_id[128];
    char phase6_migration_id[128];
    bool page_checkpoint_known;
    bool page_checkpoint_complete;
    size_t requested_page_count;
    size_t known_page_count;
    size_t unknown_page_count;
    char page_checkpoint_result[48];
    bool page_rollback_known;
    bool page_rollback_attempted;
    char page_rollback_result[48];
    size_t page_rollback_requested_count;
    size_t page_rollback_restored_count;
    size_t page_rollback_failed_count;
    size_t page_rollback_verified_count;
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
