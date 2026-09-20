#ifndef AWAVMA_MIGRATION_SAFETY_MANAGER_H
#define AWAVMA_MIGRATION_SAFETY_MANAGER_H

#include "feedback.h"
#include "benefit_classifier.h"
#include "migration.h"
#include "migration_target_provider.h"
#include "page_checkpoint.h"
#include "page_rollback.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MIGRATION_SAFETY_ACTIVE,
    MIGRATION_SAFETY_PREPARING,
    MIGRATION_SAFETY_MIGRATING,
    MIGRATION_SAFETY_VALIDATING,
    MIGRATION_SAFETY_ROLLING_BACK,
    MIGRATION_SAFETY_COMMITTED,
    MIGRATION_SAFETY_REJECTED,
    MIGRATION_SAFETY_EXECUTION_FAILED,
    MIGRATION_SAFETY_TIMEOUT,
    MIGRATION_SAFETY_ROLLBACK_SUCCEEDED,
    MIGRATION_SAFETY_ROLLBACK_FAILED,
    MIGRATION_SAFETY_COOLDOWN,
    MIGRATION_SAFETY_QUARANTINED,
    MIGRATION_SAFETY_TARGET_GONE,
    MIGRATION_SAFETY_VALIDATION_UNKNOWN,
    MIGRATION_SAFETY_EXECUTION_DISABLED,
    MIGRATION_SAFETY_SUPPRESSED,
    MIGRATION_SAFETY_TARGET_UNAVAILABLE,
    MIGRATION_SAFETY_BENEFIT_REJECTED
} MigrationSafetyState;

typedef enum {
    MIGRATION_SAFETY_HEALTHY,
    MIGRATION_SAFETY_NO_MEANINGFUL_CHANGE,
    MIGRATION_SAFETY_DEGRADED,
    MIGRATION_SAFETY_SUSPECTED_STALL,
    MIGRATION_SAFETY_VALIDATION_TARGET_GONE,
    MIGRATION_SAFETY_INSUFFICIENT_VALIDATION_DATA,
    MIGRATION_SAFETY_STRUCTURALLY_VALID,
    MIGRATION_SAFETY_STRUCTURAL_MISMATCH,
    MIGRATION_SAFETY_STRUCTURALLY_VALID_PROGRESS,
    MIGRATION_SAFETY_NO_PROGRESS_INCONCLUSIVE
} MigrationSafetyValidation;

typedef enum {
    MIGRATION_SAFETY_ROLLBACK_NOT_REQUIRED,
    MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT,
    MIGRATION_SAFETY_ROLLBACK_PARTIAL,
    MIGRATION_SAFETY_ROLLBACK_UNAVAILABLE,
    MIGRATION_SAFETY_ROLLBACK_FAILED_RESULT
} MigrationSafetyRecovery;

typedef struct {
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    ValidationAction action;
    int source_numa_node;
    int destination_numa_node;
    bool placement_available;
    bool target_valid;
    bool target_requires_cross_node;
    bool system_safe;
    bool before_metrics_available;
    double before_metrics[FEEDBACK_METRIC_COUNT];
    size_t before_samples;
    /* Supplied by the safety manager when target policy state is available. */
    bool target_policy_safety_state_available;
    bool target_policy_cooldown_active;
    bool target_policy_quarantined;
    bool target_policy_migration_intent_approved;
    bool target_policy_history_available;
    bool target_policy_recent_equivalent_failure;
    ValidationAction target_policy_previous_action;
    int target_policy_previous_source_node;
    int target_policy_previous_target_node;
    /* Attempt-local output owned by the safety manager during rollback dispatch. */
    PageRollbackSummary *page_rollback_summary;
    MigrationRequest migration_request;
} MigrationSafetyRequest;

typedef struct {
    char attempt_id[128];
    MigrationSafetyState state;
    MigrationSafetyValidation validation;
    MigrationSafetyRecovery recovery;
    MigrationResultCode execution_result;
    MigrationTargetResult target_result;
    uint64_t execution_time_ms;
    unsigned consecutive_failures;
    bool cooldown_entered;
    bool quarantined;
    bool feedback_recorded;
    bool persistence_recorded;
    bool structural_validation_known;
    bool structural_validation_succeeded;
    bool progress_known;
    bool progress_observed;
    bool benefit_known;
    BenefitClassification benefit_result;
    PageCheckpointResult page_checkpoint_result;
    size_t page_checkpoint_requested_count;
    size_t page_checkpoint_known_count;
    size_t page_checkpoint_unknown_count;
    bool page_checkpoint_complete;
    bool page_rollback_known;
    PageRollbackSummary page_rollback_summary;
    char target_reason[256];
    char benefit_reason[128];
    char detail[160];
} MigrationSafetyResult;

typedef bool (*migration_safety_identity_fn)(void *context, pid_t pid,
                                              uint64_t start_time_ticks);
typedef MigrationResultCode (*migration_safety_execute_fn)(void *context,
                                                             const MigrationRequest *request,
                                                             MigrationReport *report);
typedef bool (*migration_safety_capture_before_fn)(void *context,
                                                    const MigrationSafetyRequest *request,
                                                    const char *attempt_id);
typedef MigrationSafetyValidation (*migration_safety_validate_fn)(void *context,
                                                                     const MigrationSafetyRequest *request,
                                                                     const char *attempt_id,
                                                                     const MigrationReport *report);
typedef MigrationSafetyRecovery (*migration_safety_rollback_fn)(void *context,
                                                                  const MigrationSafetyRequest *request,
                                                                   const MigrationReport *report);
typedef MigrationTargetResult (*migration_safety_target_fn)(void *context,
                                                              const MigrationSafetyRequest *request,
                                                              const char *attempt_id,
                                                                MigrationTarget *target);
typedef PageCheckpointResult (*migration_safety_page_checkpoint_fn)(void *context,
                                                                      const MigrationSafetyRequest *request,
                                                                      const char *attempt_id,
                                                                      MigrationPageCheckpoint *checkpoint);
typedef BenefitClassification (*migration_safety_benefit_fn)(void *context,
                                                               const MigrationSafetyRequest *request,
                                                               const MigrationTarget *target,
                                                               const char *attempt_id,
                                                               BenefitDecision *decision);
typedef bool (*migration_safety_feedback_fn)(void *context, const FeedbackEvent *event,
                                              FeedbackResult *result);

typedef struct {
    bool enabled;
    bool execution_enabled;
    uint64_t execution_timeout_ms;
    uint64_t validation_window_ms;
    uint64_t cooldown_ms;
    uint64_t suppression_window_ms;
    unsigned failure_limit;
    size_t max_applications;
    const char *history_path;
    migration_safety_identity_fn identity_fn;
    migration_safety_execute_fn execute_fn;
    migration_safety_capture_before_fn capture_before_fn;
    migration_safety_validate_fn validate_fn;
    migration_safety_rollback_fn rollback_fn;
    migration_safety_target_fn target_fn;
    migration_safety_page_checkpoint_fn page_checkpoint_fn;
    migration_safety_benefit_fn benefit_fn;
    migration_safety_feedback_fn feedback_fn;
    void *callback_context;
} MigrationSafetyConfig;

typedef struct migration_safety_manager MigrationSafetyManager;

void migration_safety_config_default(MigrationSafetyConfig *config);
const char *migration_safety_state_name(MigrationSafetyState state);
MigrationSafetyManager *migration_safety_manager_create(void);
bool migration_safety_manager_init(MigrationSafetyManager *manager,
                                   const MigrationSafetyConfig *config);
bool migration_safety_manager_attempt(MigrationSafetyManager *manager,
                                      const MigrationSafetyRequest *request,
                                      MigrationSafetyResult *result);
void migration_safety_manager_shutdown(MigrationSafetyManager *manager);
void migration_safety_manager_destroy(MigrationSafetyManager *manager);

#ifdef AWAVMA_RUNTIME_TESTING
/* Test-only continuation after a valid page checkpoint and a recovery-required state. */
bool migration_safety_test_resume_page_recovery(MigrationSafetyManager *manager,
                                                 const MigrationSafetyRequest *request,
                                                 const char *active_attempt_id,
                                                 const MigrationPageCheckpoint *checkpoint,
                                                 MigrationSafetyResult *result);
#endif

#endif
