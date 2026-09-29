#include "migration_safety_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool calibrated;
    unsigned executor_calls;
    unsigned feedback_calls;
    FeedbackEvent event;
} fixture_t;

static bool identity(void *context, pid_t pid, uint64_t ticks)
{
    (void)context;
    return pid == 42 && ticks == 99;
}

static MigrationTargetResult target(void *context, const MigrationSafetyRequest *request,
                                   const char *attempt, MigrationTarget *result)
{
    (void)context;
    memset(result, 0, sizeof(*result));
    result->pid = request->pid;
    result->start_time_ticks = request->start_time_ticks;
    result->action = request->action;
    snprintf(result->attempt_id, sizeof(result->attempt_id), "%s", attempt);
    result->source_node_known = true;
    result->source_numa_node = 0;
    result->has_target_numa_node = true;
    result->target_numa_node = 1;
    result->has_target_cpu_mask = true;
    CPU_SET(2, &result->target_cpu_mask);
    return MIGRATION_TARGET_AVAILABLE;
}

static BenefitClassification benefit(void *context, const MigrationSafetyRequest *request,
                                     const MigrationTarget *target_value, const char *attempt,
                                     BenefitDecision *decision)
{
    fixture_t *fixture = context;
    BenefitClassifierInput input = {0};

    input.pid = request->pid;
    input.start_time_ticks = request->start_time_ticks;
    input.attempt_id = attempt;
    input.action = request->action;
    input.process_active = true;
    input.identity_match = true;
    input.phase5_fresh = true;
    input.phase6_fresh = true;
    input.evidence_attempt_bound = request->migration_request.benefit_evidence_bound &&
                                   strcmp(request->migration_request.benefit_evidence_attempt_id,
                                          attempt) == 0;
    input.decision = &request->migration_request.phase5_decision;
    input.validation = &request->migration_request.phase6_validation;
    input.target = target_value;
    input.target_provider_validated = true;
    input.target_online = true;
    input.target_permitted = true;
    input.source_known = true;
    input.source_target_valid = true;
    input.calibration.state = fixture->calibrated ? BENEFIT_CALIBRATION_VALIDATED_TEST_ONLY :
                                                   BENEFIT_CALIBRATION_UNAVAILABLE;
    input.calibration.provenance = fixture->calibrated ? "synthetic-runtime-fixture" : "production-default";
    return benefit_classifier_evaluate(&input, decision);
}

static MigrationResultCode execute(void *context, const MigrationRequest *request,
                                   MigrationReport *report)
{
    (void)request;
    (void)report;
    ((fixture_t *)context)->executor_calls++;
    return MIGRATION_SYSTEM_ERROR;
}

static bool feedback(void *context, const FeedbackEvent *event, FeedbackResult *result)
{
    fixture_t *fixture = context;
    fixture->feedback_calls++;
    fixture->event = *event;
    memset(result, 0, sizeof(*result));
    return true;
}

static bool run_case(const char *id, bool calibrated, MigrationSafetyState expected_state,
                     BenefitClassification expected_benefit)
{
    fixture_t fixture = {.calibrated = calibrated};
    MigrationSafetyConfig config;
    MigrationSafetyRequest request = {0};
    MigrationSafetyResult result;
    MigrationSafetyManager *manager = migration_safety_manager_create();
    bool passed;

    snprintf(request.app_id, sizeof(request.app_id), "runtime-benefit");
    request.pid = 42;
    request.start_time_ticks = 99;
    request.action = VALIDATION_ACTION_MOVE_THREAD;
    request.system_safe = true;
    request.migration_request.pid = 42;
    request.migration_request.start_time_ticks = 99;
    request.migration_request.start_time_ticks_available = true;
    request.migration_request.phase5_decision.pid = 42;
    request.migration_request.phase5_decision.action = VALIDATION_ACTION_MOVE_THREAD;
    request.migration_request.phase5_decision.evidence_model = DECISION_EVIDENCE_UTILITY_POLICY;
    request.migration_request.phase5_decision.phase5_evidence_available = true;
    request.migration_request.phase5_decision.phase5_utility_available = true;
    request.migration_request.phase5_decision.phase5_decision_margin_available = true;
    request.migration_request.phase5_decision.phase5_epsilon_available = true;
    request.migration_request.phase5_decision.phase5_versions_available = true;
    request.migration_request.phase5_decision.phase5_memory_score_final = 0.0;
    request.migration_request.phase5_decision.phase5_thread_score_final = 0.24;
    request.migration_request.phase5_decision.phase5_decision_margin = -0.24;
    request.migration_request.phase5_decision.phase5_epsilon = 0.05;
    request.migration_request.phase5_decision.phase5_weight_version = 1;
    request.migration_request.phase5_decision.phase5_bias_version = 1;
    snprintf(request.migration_request.phase5_decision.phase5_decision_status,
             sizeof(request.migration_request.phase5_decision.phase5_decision_status), "DECISION_VALID");
    request.migration_request.phase6_validation.pid = 42;
    request.migration_request.phase6_validation.action = VALIDATION_ACTION_MOVE_THREAD;
    request.migration_request.phase6_validation.confidence_status = GATE_PASS;
    request.migration_request.phase6_validation.roi_status = GATE_NOT_APPLICABLE;
    request.migration_request.phase6_validation.confidence_score = 80.0;
    request.migration_request.phase6_validation.roi_score = -1.0;
    request.migration_request.phase6_validation.safety_status = GATE_PASS;
    request.migration_request.phase6_validation.safety_score = 80.0;
    snprintf(request.migration_request.phase6_validation.final_decision,
             sizeof(request.migration_request.phase6_validation.final_decision), "APPROVED");
    migration_safety_config_default(&config);
    config.enabled = true;
    config.execution_enabled = false;
    config.identity_fn = identity;
    config.target_fn = target;
    config.benefit_fn = benefit;
    config.execute_fn = execute;
    config.feedback_fn = feedback;
    config.callback_context = &fixture;
    passed = manager != NULL && migration_safety_manager_init(manager, &config) &&
             migration_safety_manager_attempt(manager, &request, &result) &&
             result.state == expected_state && result.benefit_result == expected_benefit &&
             fixture.executor_calls == 0 && fixture.feedback_calls == 1 &&
             fixture.event.benefit_classification == expected_benefit;
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
    migration_safety_manager_destroy(manager);
    return passed;
}

int main(void)
{
    bool passed = run_case("RBC01_SYNTHETIC_SUPPORTED_EXECUTION_DISABLED", true,
                           MIGRATION_SAFETY_EXECUTION_DISABLED, BENEFIT_SUPPORTED);
    passed = run_case("RBC02_PRODUCTION_UNCALIBRATED_REJECTED", false,
                      MIGRATION_SAFETY_BENEFIT_REJECTED, BENEFIT_POLICY_UNCALIBRATED) && passed;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
