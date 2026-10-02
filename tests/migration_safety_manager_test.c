#define _POSIX_C_SOURCE 200809L

#include "migration_safety_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    bool identity_ok;
    MigrationResultCode execution;
    MigrationMutationState mutation_state;
    MigrationSafetyValidation validation;
    MigrationSafetyRecovery recovery;
    unsigned rollback_calls;
    unsigned feedback_count;
    FeedbackTerminalOutcome outcome;
    bool sleep_for_timeout;
    unsigned identity_calls;
    unsigned identity_fail_after;
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
    report->mutation_state = context->mutation_state;
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
    if (request->action == VALIDATION_ACTION_MOVE_MEMORY && request->page_rollback_summary != NULL) {
        request->page_rollback_summary->result = PAGE_ROLLBACK_COMPLETE;
        request->page_rollback_summary->attempted = true;
        request->page_rollback_summary->requested_count = request->migration_request.page_count;
        request->page_rollback_summary->restored_count = request->migration_request.page_count;
        request->page_rollback_summary->verified_count = request->migration_request.page_count;
    }
    return context->recovery;
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
    context.mutation_state = MIGRATION_VERIFIED_SUCCESS;
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

static PageCheckpointResult complete_page_checkpoint(void *opaque,
                                                     const MigrationSafetyRequest *request,
                                                     const char *attempt_id,
                                                     MigrationPageCheckpoint *checkpoint)
{
    (void)opaque;
    (void)attempt_id;
    memset(checkpoint, 0, sizeof(*checkpoint));
    checkpoint->complete = true;
    checkpoint->requested_count = request->migration_request.page_count;
    checkpoint->known_count = request->migration_request.page_count;
    return PAGE_CHECKPOINT_COMPLETE;
}

static MigrationSafetyRequest valid_page_request(const char *app_id)
{
    static void *page = (void *)(uintptr_t)0x1000;
    MigrationSafetyRequest request = valid_request(app_id);

    request.action = VALIDATION_ACTION_MOVE_MEMORY;
    request.placement_available = true;
    request.target_valid = true;
    request.system_safe = true;
    request.source_numa_node = 10;
    request.destination_numa_node = 11;
    request.migration_request.phase5_decision.action = VALIDATION_ACTION_MOVE_MEMORY;
    request.migration_request.source_numa_node = request.source_numa_node;
    request.migration_request.destination_numa_node = request.destination_numa_node;
    request.migration_request.numa_nodes_available = true;
    request.migration_request.pages = &page;
    request.migration_request.page_count = 1;
    request.migration_request.page_metadata_available = true;
    request.migration_request.page_addresses_authoritative = true;
    request.migration_request.memory_region_verified = true;
    return request;
}

static bool mutation_state_cases(MigrationSafetyConfig *config)
{
    test_context_t context = {.identity_ok = true, .execution = MIGRATION_SYSTEM_ERROR,
                              .mutation_state = MIGRATION_NO_MUTATION,
                              .validation = MIGRATION_SAFETY_HEALTHY,
                              .recovery = MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT};
    MigrationSafetyManager *manager = migration_safety_manager_create();
    MigrationSafetyRequest request = valid_request("MS12_MUTATION_STATE");
    MigrationSafetyResult no_mutation = {0}, possible = {0}, partial = {0}, rollback_failed = {0},
                          page_partial = {0};
    size_t previous_max_applications = config->max_applications;
    bool passed, case_pass;

    config->callback_context = &context;
    config->failure_limit = 10;
    config->cooldown_ms = 0;
    config->suppression_window_ms = 0;
    config->max_applications = 5;
    config->page_checkpoint_fn = complete_page_checkpoint;
    if (manager == NULL || !migration_safety_manager_init(manager, config)) {
        migration_safety_manager_destroy(manager);
        config->page_checkpoint_fn = NULL;
        config->max_applications = previous_max_applications;
        return false;
    }
    case_pass = migration_safety_manager_attempt(manager, &request, &no_mutation) &&
                no_mutation.state == MIGRATION_SAFETY_EXECUTION_FAILED &&
                no_mutation.execution_result == MIGRATION_SYSTEM_ERROR && context.rollback_calls == 0;
    printf("MS12_DIAGNOSTIC state=%d execution=%d recovery=%d rollbacks=%u %s\n",
           no_mutation.state, no_mutation.execution_result, no_mutation.recovery,
           context.rollback_calls, case_pass ? "PASS" : "FAIL");
    passed = case_pass;
    context.mutation_state = MIGRATION_MUTATION_POSSIBLE;
    request = valid_request("MS13_MUTATION_POSSIBLE");
    case_pass = migration_safety_manager_attempt(manager, &request, &possible) &&
                possible.state == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED &&
                possible.execution_result == MIGRATION_SYSTEM_ERROR &&
                possible.recovery == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT &&
                context.rollback_calls == 1;
    printf("MS13_DIAGNOSTIC state=%d execution=%d recovery=%d rollbacks=%u %s\n",
           possible.state, possible.execution_result, possible.recovery, context.rollback_calls,
           case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    context.execution = MIGRATION_PARTIAL_SUCCESS;
    request = valid_request("MS14_PARTIAL_MUTATION");
    case_pass = migration_safety_manager_attempt(manager, &request, &partial) &&
                partial.state == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED &&
                partial.execution_result == MIGRATION_PARTIAL_SUCCESS && context.rollback_calls == 2;
    printf("MS14_DIAGNOSTIC state=%d execution=%d recovery=%d rollbacks=%u %s\n",
           partial.state, partial.execution_result, partial.recovery, context.rollback_calls,
           case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    context.recovery = MIGRATION_SAFETY_ROLLBACK_FAILED_RESULT;
    request = valid_request("MS15_ROLLBACK_FAILED");
    case_pass = migration_safety_manager_attempt(manager, &request, &rollback_failed) &&
                rollback_failed.state == MIGRATION_SAFETY_ROLLBACK_FAILED &&
                rollback_failed.execution_result == MIGRATION_PARTIAL_SUCCESS &&
                rollback_failed.recovery == MIGRATION_SAFETY_ROLLBACK_FAILED_RESULT &&
                context.rollback_calls == 3;
    printf("MS15_DIAGNOSTIC state=%d execution=%d recovery=%d rollbacks=%u %s\n",
           rollback_failed.state, rollback_failed.execution_result, rollback_failed.recovery,
           context.rollback_calls, case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    context.recovery = MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT;
    request = valid_page_request("MS16_PAGE_PARTIAL_MUTATION");
    case_pass = migration_safety_manager_attempt(manager, &request, &page_partial) &&
                page_partial.state == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED &&
                page_partial.execution_result == MIGRATION_PARTIAL_SUCCESS &&
                page_partial.page_checkpoint_complete && page_partial.page_rollback_known &&
                page_partial.page_rollback_summary.result == PAGE_ROLLBACK_COMPLETE &&
                context.rollback_calls == 4;
    printf("MS16_DIAGNOSTIC state=%d execution=%d recovery=%d rollbacks=%u checkpoint=%s "
           "page_rollback=%d %s\n", page_partial.state, page_partial.execution_result,
           page_partial.recovery, context.rollback_calls,
           page_partial.page_checkpoint_complete ? "true" : "false",
           page_partial.page_rollback_summary.result, case_pass ? "PASS" : "FAIL");
    passed = passed && case_pass;
    config->page_checkpoint_fn = NULL;
    config->max_applications = previous_max_applications;
    printf("MS12_16_MUTATION_STATE_ROLLBACK: %s\n", passed ? "PASS" : "FAIL");
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
    passed = strcmp(migration_safety_state_name(MIGRATION_SAFETY_BENEFIT_REJECTED),
                    "BENEFIT_REJECTED") == 0;
    printf("MS00_BENEFIT_REJECTED_STATE_NAME | %s\n", passed ? "PASS" : "FAIL");
    passed = circuit_breaker_cases(&config) && passed;
    passed = suppression_case(&config) && passed;
    passed = mutation_state_cases(&config) && passed;

#define CASE(id, setup, expected_state, expected_outcome) do { \
        memset(&context, 0, sizeof(context)); \
        context.identity_ok = true; \
        context.execution = MIGRATION_SUCCESS; \
        context.mutation_state = MIGRATION_VERIFIED_SUCCESS; \
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
    CASE("MS03_EXECUTION_FAILURE", context.execution = MIGRATION_SYSTEM_ERROR; context.mutation_state = MIGRATION_NO_MUTATION, MIGRATION_SAFETY_EXECUTION_FAILED, FEEDBACK_TERMINAL_EXECUTION_FAILED);
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
