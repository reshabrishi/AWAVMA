#ifndef AWAVMA_RUNTIME_H
#define AWAVMA_RUNTIME_H

#include "runtime_monitor.h"
#include "benefit_classifier.h"
#include "page_checkpoint.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AWAVMA_RUNTIME_MONITORING,
    AWAVMA_RUNTIME_INSUFFICIENT,
    AWAVMA_RUNTIME_REJECTED,
    AWAVMA_RUNTIME_MIGRATION_METADATA_UNAVAILABLE,
    AWAVMA_RUNTIME_FEEDBACK_PENDING,
    AWAVMA_RUNTIME_TARGET_GONE,
    AWAVMA_RUNTIME_ERROR,
    AWAVMA_RUNTIME_AMBIGUOUS
} awavma_runtime_status_t;

typedef enum {
    AWAVMA_PHASE4_SUBPROCESS,
    AWAVMA_PHASE4_IN_PROCESS
} awavma_phase4_mode_t;

typedef struct {
    uint64_t monitor_interval_ms;
    uint64_t discovery_interval_ms;
    uint64_t evaluation_interval_ms;
    size_t max_applications;
    size_t worker_count;
    size_t queue_capacity;
    awavma_phase4_mode_t phase4_mode;
    const char *root_dir;
    const char *bin_dir;
    const char *phase_config_path;
    bool migration_safety_enabled;
    bool migration_execution_enabled;
    BenefitCalibrationState benefit_calibration_state;
    const char *benefit_calibration_provenance;
    application_discovery_config_t discovery_config;
    runtime_monitor_application_filter_fn application_filter;
    void *application_filter_context;
} awavma_runtime_config_t;

typedef struct {
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    uint64_t generation;
    size_t phase3_samples;
    size_t phase5_committed_rows;
    awavma_runtime_status_t status;
    char detail[128];
} awavma_runtime_record_t;

typedef struct awavma_runtime awavma_runtime_t;

void awavma_runtime_config_default(awavma_runtime_config_t *config);
const char *awavma_runtime_status_name(awavma_runtime_status_t status);
awavma_runtime_t *awavma_runtime_create(void);
int awavma_runtime_init(awavma_runtime_t *runtime,
                        const awavma_runtime_config_t *config);
int awavma_runtime_run_for(awavma_runtime_t *runtime, uint64_t duration_ms);
void awavma_runtime_request_stop(awavma_runtime_t *runtime);
size_t awavma_runtime_snapshot(const awavma_runtime_t *runtime,
                               awavma_runtime_record_t *records,
                               size_t capacity);
void awavma_runtime_shutdown(awavma_runtime_t *runtime);
void awavma_runtime_destroy(awavma_runtime_t *runtime);

#ifdef AWAVMA_RUNTIME_TESTING
typedef enum {
    AWAVMA_RUNTIME_TEST_TARGET_NO_ALTERNATE,
    AWAVMA_RUNTIME_TEST_TARGET_UNAVAILABLE,
    AWAVMA_RUNTIME_TEST_TARGET_INVALID,
    AWAVMA_RUNTIME_TEST_TARGET_VALID,
    AWAVMA_RUNTIME_TEST_TARGET_PLACEMENT_MISMATCH,
    AWAVMA_RUNTIME_TEST_TARGET_CAPTURE_FAILURE,
    AWAVMA_RUNTIME_TEST_TARGET_IDENTITY_MISMATCH_AFTER_EXEC,
    /* Runs the production policy with live metadata; no target is injected. */
    AWAVMA_RUNTIME_TEST_TARGET_POLICY_LIVE,
    /* Exercises the production page-checkpoint gate with no injected addresses. */
    AWAVMA_RUNTIME_TEST_TARGET_PAGE_ADDRESS_UNAVAILABLE
} awavma_runtime_test_target_case_t;

typedef struct {
    unsigned target_provider_calls;
    unsigned executor_calls;
    unsigned rollback_calls;
    unsigned validation_before_calls;
    unsigned validation_after_calls;
    unsigned terminal_feedback_calls;
    bool structural_validation_committed;
    bool structural_validation_known;
    bool structural_validation_succeeded;
    bool progress_known;
    bool progress_observed;
    bool benefit_known;
    BenefitClassification benefit_classification;
    char benefit_reason[128];
    bool before_cpu_time_available;
    uint64_t before_cpu_time_ticks;
    bool after_cpu_time_available;
    uint64_t after_cpu_time_ticks;
    bool rollback_succeeded;
    char attempt_id[128];
} awavma_runtime_test_target_stats_t;

/* Test-only upstream decision entry; it preserves the production runtime callbacks. */
int awavma_runtime_test_submit_approved_migration(
    awavma_runtime_t *runtime, pid_t pid, uint64_t start_time_ticks,
    awavma_runtime_test_target_case_t target_case,
    awavma_runtime_test_target_stats_t *stats);
int awavma_runtime_test_resume_page_recovery(
    awavma_runtime_t *runtime, const char *active_attempt_id,
    const MigrationPageCheckpoint *checkpoint,
    awavma_runtime_test_target_stats_t *stats);
/* Test-only adapter and join entry points for Phase 5/6 evidence fixtures. */
int awavma_runtime_test_write_validation_input(const char *decision_path, const char *output_path,
                                               const awavma_runtime_record_t *record);
int awavma_runtime_test_load_benefit_evidence(const char *validation_input_path,
                                              const char *validation_path,
                                              const awavma_runtime_record_t *record,
                                              DecisionData *decision, ValidationResult *validation);
#endif

#endif
