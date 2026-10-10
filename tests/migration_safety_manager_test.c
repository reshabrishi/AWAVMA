#define _POSIX_C_SOURCE 200809L

#include "migration_safety_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    bool identity_ok;
    MigrationResultCode execution;
    MigrationSafetyValidation validation;
    MigrationSafetyRecovery recovery;
    unsigned feedback_count;
    FeedbackTerminalOutcome outcome;
    bool sleep_for_timeout;
    unsigned identity_calls;
    unsigned identity_fail_after;
    bool mutation_attempted;
    bool mutation_observed;
    bool mutation_indeterminate;
    unsigned rollback_calls;
} test_context_t;

static bool identity(void *opaque, pid_t pid, uint64_t ticks)
{
    test_context_t *context = opaque;
    context->identity_calls++;
    return context->identity_ok && pid == 4242 && ticks == 1234 &&
           (context->identity_fail_after == 0 || context->identity_calls < context->identity_fail_after);
}

static MigrationResultCode execute(void *opaque, const MigrationRequest *request, MigrationReport *report)
{
    test_context_t *context = opaque;

    (void)request;
    if (context->sleep_for_timeout) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 2000000};
        nanosleep(&delay, NULL);
    }
    memset(report, 0, sizeof(*report));
    report->result = context->execution;
    report->mutation_attempted = context->mutation_attempted;
    report->mutation_observed = context->mutation_observed;
    report->mutation_indeterminate = context->mutation_indeterminate;
    return context->execution;
}

static MigrationSafetyValidation validate(void *opaque, const MigrationSafetyRequest *request,
                                            const char *attempt_id,
                                            const MigrationReport *report)
{
    test_context_t *context = opaque;

    (void)request;
    (void)attempt_id;
    (void)report;
    return context->validation;
}

static MigrationSafetyRecovery rollback(void *opaque, const MigrationSafetyRequest *request,
                                        const MigrationReport *report)
{
    test_context_t *context = opaque;

    (void)request;
    (void)report;
    context->rollback_calls++;
    return context->recovery;
}

static PageCheckpointResult checkpoint(void *opaque, const MigrationSafetyRequest *request,
                                       const char *attempt_id, MigrationPageCheckpoint *value)
{
    (void)opaque;
    memset(value, 0, sizeof(*value));
    value->entries = calloc(1, sizeof(*value->entries));
    if (value->entries == NULL)
        return PAGE_CHECKPOINT_ALLOCATION_FAILED;
    value->pid = request->pid;
    value->start_time_ticks = request->start_time_ticks;
    snprintf(value->attempt_id, sizeof(value->attempt_id), "%s", attempt_id);
    value->requested_count = value->known_count = 1;
    value->complete = true;
    value->result = PAGE_CHECKPOINT_COMPLETE;
    return value->result;
}

static bool feedback(void *opaque, const FeedbackEvent *event, FeedbackResult *result)
{
    test_context_t *context = opaque;

    context->feedback_count++;
    context->outcome = event->terminal_outcome;
    memset(result, 0, sizeof(*result));
    result->update_status = FEEDBACK_UPDATE_TERMINAL_RECORDED;
    return event->event_kind == FEEDBACK_EVENT_MIGRATION_TERMINAL;
}

static MigrationSafetyRequest valid_request(const char *app_id)
{
    MigrationSafetyRequest request;

    memset(&request, 0, sizeof(request));
    snprintf(request.app_id, sizeof(request.app_id), "%s", app_id);
    request.pid = 4242;
    request.start_time_ticks = 1234;
    request.action = VALIDATION_ACTION_MOVE_THREAD;
    request.source_numa_node = 0;
    request.destination_numa_node = 1;
    request.placement_available = true;
    request.target_valid = true;
    request.system_safe = true;
    request.migration_request.start_time_ticks_available = true;
    request.migration_request.start_time_ticks = request.start_time_ticks;
    request.migration_request.pid = request.pid;
    request.migration_request.phase5_decision.action = request.action;
    snprintf(request.migration_request.phase5_decision.entity_id,
             sizeof(request.migration_request.phase5_decision.entity_id), "thread-4242");
    snprintf(request.migration_request.phase6_validation.final_decision,
             sizeof(request.migration_request.phase6_validation.final_decision), "APPROVED");
    return request;
}

static MigrationSafetyRequest valid_memory_request(const char *app_id)
{
    static void *page = (void *)4096;
    MigrationSafetyRequest request = valid_request(app_id);

    request.action = VALIDATION_ACTION_MOVE_MEMORY;
    request.migration_request.phase5_decision.action = request.action;
    request.migration_request.pages = &page;
    request.migration_request.page_count = 1;
    request.migration_request.page_metadata_available = true;
    request.migration_request.page_addresses_authoritative = true;
    request.migration_request.memory_region_verified = true;
    return request;
}

static bool memory_recovery_cases(MigrationSafetyConfig *config)
{
    test_context_t context = {.identity_ok = true, .execution = MIGRATION_SUCCESS,
                              .validation = MIGRATION_SAFETY_STRUCTURAL_MISMATCH,
                              .recovery = MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT,
                              .mutation_attempted = true, .mutation_observed = true};
    MigrationSafetyManager *manager;
    MigrationSafetyRequest request;
    MigrationSafetyResult result;
    bool passed = true;

    config->page_checkpoint_fn = checkpoint;
    config->callback_context = &context;
    request = valid_memory_request("MS12_POST_EXEC_IDENTITY");
    context.identity_fail_after = 2;
    manager = migration_safety_manager_create();
    passed = manager != NULL && migration_safety_manager_init(manager, config) &&
             migration_safety_manager_attempt(manager, &request, &result) &&
             result.state == MIGRATION_SAFETY_QUARANTINED && result.quarantined &&
             context.rollback_calls == 0;
    migration_safety_manager_destroy(manager);
    printf("MS12_POST_EXEC_IDENTITY_NO_ROLLBACK | %s\n", passed ? "PASS" : "FAIL");

    memset(&context, 0, sizeof(context));
    context.identity_ok = true;
    context.execution = MIGRATION_SUCCESS;
    context.validation = MIGRATION_SAFETY_STRUCTURAL_MISMATCH;
    context.recovery = MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT;
    context.mutation_attempted = context.mutation_observed = true;
    config->callback_context = &context;
    request = valid_memory_request("MS13_VERIFICATION_ROLLBACK");
    manager = migration_safety_manager_create();
    passed = manager != NULL && migration_safety_manager_init(manager, config) &&
             migration_safety_manager_attempt(manager, &request, &result) &&
             result.state == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED && context.rollback_calls == 1 && passed;
    migration_safety_manager_destroy(manager);
    printf("MS13_VERIFICATION_FAILURE_ROLLBACK | %s\n", context.rollback_calls == 1 ? "PASS" : "FAIL");

    context.execution = MIGRATION_SYSTEM_ERROR;
    context.validation = MIGRATION_SAFETY_HEALTHY;
    context.mutation_observed = true;
    context.mutation_indeterminate = false;
    context.rollback_calls = 0;
    request = valid_memory_request("MS14_PARTIAL_MUTATION");
    manager = migration_safety_manager_create();
    passed = manager != NULL && migration_safety_manager_init(manager, config) &&
             migration_safety_manager_attempt(manager, &request, &result) &&
             result.state == MIGRATION_SAFETY_EXECUTION_FAILED && context.rollback_calls == 1 && passed;
    migration_safety_manager_destroy(manager);
    printf("MS14_PARTIAL_MUTATION_ROLLBACK | %s\n", context.rollback_calls == 1 ? "PASS" : "FAIL");

    context.mutation_observed = false;
    context.rollback_calls = 0;
    request = valid_memory_request("MS15_PROVEN_NO_MUTATION");
    manager = migration_safety_manager_create();
    passed = manager != NULL && migration_safety_manager_init(manager, config) &&
             migration_safety_manager_attempt(manager, &request, &result) &&
             result.state == MIGRATION_SAFETY_EXECUTION_FAILED && context.rollback_calls == 0 && passed;
    migration_safety_manager_destroy(manager);
    printf("MS15_PROVEN_NO_MUTATION_NO_ROLLBACK | %s\n", context.rollback_calls == 0 ? "PASS" : "FAIL");
    config->page_checkpoint_fn = NULL;
    return passed;
}

static bool run_case(const char *name, test_context_t *context, MigrationSafetyRequest *request,
                     MigrationSafetyState expected_state, FeedbackTerminalOutcome expected_outcome,
                     MigrationSafetyConfig *config)
{
    MigrationSafetyManager *manager = migration_safety_manager_create();
    MigrationSafetyResult result;
    bool passed = manager != NULL && migration_safety_manager_init(manager, config) &&
                  migration_safety_manager_attempt(manager, request, &result) &&
                  result.state == expected_state && result.feedback_recorded &&
                  context->feedback_count == 1 && context->outcome == expected_outcome;

    printf("%s | %s\n", name, passed ? "PASS" : "FAIL");
    migration_safety_manager_destroy(manager);
    return passed;
}

static bool circuit_breaker_cases(MigrationSafetyConfig *config)
{
    test_context_t context = {.identity_ok = true, .execution = MIGRATION_SYSTEM_ERROR,
                              .validation = MIGRATION_SAFETY_HEALTHY,
                              .recovery = MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT};
    MigrationSafetyManager *manager = migration_safety_manager_create();
    MigrationSafetyRequest request = valid_request("MS10_APP_A");
    MigrationSafetyResult first, second, third;
    bool passed;

    config->callback_context = &context;
    config->failure_limit = 1;
    config->cooldown_ms = 0;
    config->suppression_window_ms = 0;
    if (manager == NULL || !migration_safety_manager_init(manager, config)) {
        migration_safety_manager_destroy(manager);
        return false;
    }
    passed = migration_safety_manager_attempt(manager, &request, &first) &&
             first.state == MIGRATION_SAFETY_COOLDOWN && first.feedback_recorded &&
             migration_safety_manager_attempt(manager, &request, &second) &&
             second.state == MIGRATION_SAFETY_QUARANTINED && second.feedback_recorded;
    context.execution = MIGRATION_SUCCESS;
    request = valid_request("MS10_APP_B");
    passed = passed && migration_safety_manager_attempt(manager, &request, &third) &&
             third.state == MIGRATION_SAFETY_COMMITTED && context.feedback_count == 3;
    printf("MS10_COOLDOWN_QUARANTINE_ISOLATION | %s\n", passed ? "PASS" : "FAIL");
    migration_safety_manager_destroy(manager);
    return passed;
}

static bool suppression_case(MigrationSafetyConfig *config)
{
    test_context_t context = {.identity_ok = true, .execution = MIGRATION_SYSTEM_ERROR,
                              .validation = MIGRATION_SAFETY_HEALTHY,
                              .recovery = MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT};
    MigrationSafetyManager *manager = migration_safety_manager_create();
    MigrationSafetyRequest request = valid_request("MS11_SUPPRESSION");
    MigrationSafetyResult first, second;
    bool passed;

    config->callback_context = &context;
    config->failure_limit = 3;
    config->cooldown_ms = 0;
    config->suppression_window_ms = 30000;
    if (manager == NULL || !migration_safety_manager_init(manager, config)) {
        migration_safety_manager_destroy(manager);
        return false;
    }
    passed = migration_safety_manager_attempt(manager, &request, &first) &&
             first.state == MIGRATION_SAFETY_EXECUTION_FAILED &&
             migration_safety_manager_attempt(manager, &request, &second) &&
             second.state == MIGRATION_SAFETY_SUPPRESSED && second.feedback_recorded &&
             context.feedback_count == 2;
    printf("MS11_REPEATED_ACTION_SUPPRESSION | %s\n", passed ? "PASS" : "FAIL");
    migration_safety_manager_destroy(manager);
    return passed;
}

int main(void)
{
    MigrationSafetyConfig config;
    MigrationSafetyRequest request;
    test_context_t context;
    bool passed = true;

    migration_safety_config_default(&config);
    config.enabled = true;
    config.execution_enabled = true;

    config.max_applications = 4;
    config.failure_limit = 3;
    config.identity_fn = identity;
    config.execute_fn = execute;
    config.validate_fn = validate;
    config.rollback_fn = rollback;
    config.feedback_fn = feedback;
    passed = circuit_breaker_cases(&config) && passed;
    passed = suppression_case(&config) && passed;
    passed = memory_recovery_cases(&config) && passed;

#define CASE(id, setup, expected_state, expected_outcome) do { \
        memset(&context, 0, sizeof(context)); \
        context.identity_ok = true; \
        context.execution = MIGRATION_SUCCESS; \
        context.validation = MIGRATION_SAFETY_HEALTHY; \
        context.recovery = MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT; \
        setup; \
        request = valid_request(id); \
        config.callback_context = &context; \
        passed = run_case(id, &context, &request, expected_state, expected_outcome, &config) && passed; \
    } while (0)

    CASE("MS01_COMMITTED", (void)0, MIGRATION_SAFETY_COMMITTED, FEEDBACK_TERMINAL_COMMITTED);
    CASE("MS02_IDENTITY", context.identity_ok = false, MIGRATION_SAFETY_TARGET_GONE, FEEDBACK_TERMINAL_TARGET_GONE);
    CASE("MS02B_POST_EXEC_IDENTITY", context.identity_fail_after = 2, MIGRATION_SAFETY_TARGET_GONE, FEEDBACK_TERMINAL_TARGET_GONE);
    CASE("MS03_EXECUTION_FAILURE", context.execution = MIGRATION_SYSTEM_ERROR, MIGRATION_SAFETY_EXECUTION_FAILED, FEEDBACK_TERMINAL_EXECUTION_FAILED);
    CASE("MS04_DEGRADED", context.validation = MIGRATION_SAFETY_DEGRADED, MIGRATION_SAFETY_ROLLBACK_SUCCEEDED, FEEDBACK_TERMINAL_ROLLBACK_SUCCEEDED);
    CASE("MS05_ROLLBACK_FAILURE", context.validation = MIGRATION_SAFETY_DEGRADED; context.recovery = MIGRATION_SAFETY_ROLLBACK_FAILED_RESULT, MIGRATION_SAFETY_ROLLBACK_FAILED, FEEDBACK_TERMINAL_ROLLBACK_FAILED);
    CASE("MS06_STALL", context.validation = MIGRATION_SAFETY_SUSPECTED_STALL, MIGRATION_SAFETY_ROLLBACK_SUCCEEDED, FEEDBACK_TERMINAL_ROLLBACK_SUCCEEDED);
    CASE("MS07_UNKNOWN", context.validation = MIGRATION_SAFETY_INSUFFICIENT_VALIDATION_DATA, MIGRATION_SAFETY_VALIDATION_UNKNOWN, FEEDBACK_TERMINAL_VALIDATION_UNKNOWN);
    CASE("MS08_TIMEOUT", context.sleep_for_timeout = true; config.execution_timeout_ms = 1, MIGRATION_SAFETY_TIMEOUT, FEEDBACK_TERMINAL_TIMEOUT);
    config.execution_timeout_ms = 1000;

    memset(&context, 0, sizeof(context));
    context.identity_ok = true;
    request = valid_request("MS09_DISABLED");
    config.execution_enabled = false;
    config.callback_context = &context;
    passed = run_case("MS09_DISABLED", &context, &request, MIGRATION_SAFETY_EXECUTION_DISABLED,
                      FEEDBACK_TERMINAL_EXECUTION_DISABLED, &config) && passed;
    config.execution_enabled = true;

    puts(passed ? "Migration safety manager tests: PASS" : "Migration safety manager tests: FAIL");
    return passed ? 0 : 1;
}
