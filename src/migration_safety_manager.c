#define _POSIX_C_SOURCE 200809L

#include "migration_safety_manager.h"
#include "monitor_profile.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    bool used;
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    uint64_t sequence;
    uint64_t cooldown_until_ms;
    unsigned consecutive_failures;
    unsigned attempts;
    unsigned successes;
    unsigned failures;
    unsigned rollbacks;
    unsigned rollback_failures;
    unsigned timeouts;
    unsigned rejections;
    unsigned quarantines;
    bool quarantined;
    ValidationAction last_action;
    int last_source;
    int last_target;
    uint64_t last_failure_ms;
} app_safety_state_t;

struct migration_safety_manager {
    MigrationSafetyConfig config;
    app_safety_state_t *applications;
    size_t application_count;
};

static uint64_t now_ms(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool current_identity_matches(pid_t pid, uint64_t expected_ticks)
{
    char path[64];
    char line[4096];
    char *cursor;
    char *save = NULL;
    FILE *file;

    if (pid <= 0 || expected_ticks == 0 ||
        snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path))
        return false;
    file = fopen(path, "r");
    if (file == NULL)
        return false;
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }
    fclose(file);
    cursor = strrchr(line, ')');
    if (cursor == NULL || cursor[1] != ' ')
        return false;
    cursor += 2;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor : NULL, " ", &save);
        char *end = NULL;

        if (token == NULL)
            return false;
        if (field == 22) {
            unsigned long long ticks = strtoull(token, &end, 10);
            return end != token && (*end == '\0' || *end == '\n') && ticks == expected_ticks;
        }
    }
    return false;
}

static bool identity_matches(const MigrationSafetyManager *manager, pid_t pid, uint64_t ticks)
{
    if (manager->config.identity_fn == NULL)
        return current_identity_matches(pid, ticks);
    return manager->config.identity_fn(manager->config.callback_context, pid, ticks);
}

static app_safety_state_t *app_state(MigrationSafetyManager *manager,
                                     const MigrationSafetyRequest *request)
{
    for (size_t index = 0; index < manager->application_count; index++)
        if (manager->applications[index].pid == request->pid &&
            manager->applications[index].start_time_ticks == request->start_time_ticks &&
            strcmp(manager->applications[index].app_id, request->app_id) == 0)
            return &manager->applications[index];
    if (manager->application_count >= manager->config.max_applications)
        return NULL;
    app_safety_state_t *state = &manager->applications[manager->application_count++];

    memset(state, 0, sizeof(*state));
    state->used = true;
    state->pid = request->pid;
    state->start_time_ticks = request->start_time_ticks;
    snprintf(state->app_id, sizeof(state->app_id), "%s", request->app_id);
    return state;
}

static FeedbackTerminalOutcome feedback_outcome(const MigrationSafetyResult *result)
{
    if (result->state == MIGRATION_SAFETY_TARGET_UNAVAILABLE) {
        switch (result->target_result) {
        case MIGRATION_TARGET_NO_ALTERNATE_TARGET: return FEEDBACK_TERMINAL_NO_ALTERNATE_TARGET;
        case MIGRATION_TARGET_INVALID: return FEEDBACK_TERMINAL_TARGET_INVALID;
        case MIGRATION_TARGET_STALE: return FEEDBACK_TERMINAL_TARGET_STALE;
        case MIGRATION_TARGET_IDENTITY_CHANGED: return FEEDBACK_TERMINAL_TARGET_IDENTITY_MISMATCH;
        case MIGRATION_TARGET_TOPOLOGY_UNAVAILABLE: return FEEDBACK_TERMINAL_TARGET_TOPOLOGY_UNAVAILABLE;
        default: return FEEDBACK_TERMINAL_TARGET_UNAVAILABLE;
        }
    }
    switch (result->state) {
    case MIGRATION_SAFETY_COMMITTED: return FEEDBACK_TERMINAL_COMMITTED;
    case MIGRATION_SAFETY_REJECTED: return FEEDBACK_TERMINAL_REJECTED;
    case MIGRATION_SAFETY_EXECUTION_FAILED: return FEEDBACK_TERMINAL_EXECUTION_FAILED;
    case MIGRATION_SAFETY_TIMEOUT: return FEEDBACK_TERMINAL_TIMEOUT;
    case MIGRATION_SAFETY_ROLLBACK_SUCCEEDED: return FEEDBACK_TERMINAL_ROLLBACK_SUCCEEDED;
    case MIGRATION_SAFETY_ROLLBACK_FAILED: return FEEDBACK_TERMINAL_ROLLBACK_FAILED;
    case MIGRATION_SAFETY_COOLDOWN: return FEEDBACK_TERMINAL_COOLDOWN;
    case MIGRATION_SAFETY_QUARANTINED: return FEEDBACK_TERMINAL_QUARANTINED;
    case MIGRATION_SAFETY_TARGET_GONE: return FEEDBACK_TERMINAL_TARGET_GONE;
    case MIGRATION_SAFETY_VALIDATION_UNKNOWN: return FEEDBACK_TERMINAL_VALIDATION_UNKNOWN;
    case MIGRATION_SAFETY_EXECUTION_DISABLED: return FEEDBACK_TERMINAL_EXECUTION_DISABLED;
    case MIGRATION_SAFETY_SUPPRESSED: return FEEDBACK_TERMINAL_SUPPRESSED;
    case MIGRATION_SAFETY_BENEFIT_REJECTED: return FEEDBACK_TERMINAL_REJECTED;
    default: return FEEDBACK_TERMINAL_EXECUTION_FAILED;
    }
}

static bool is_failure(MigrationSafetyState state)
{
    return state == MIGRATION_SAFETY_EXECUTION_FAILED || state == MIGRATION_SAFETY_TIMEOUT ||
           state == MIGRATION_SAFETY_ROLLBACK_FAILED || state == MIGRATION_SAFETY_TARGET_GONE ||
           state == MIGRATION_SAFETY_VALIDATION_UNKNOWN;
}

static bool append_history(const MigrationSafetyManager *manager,
                           const MigrationSafetyRequest *request,
                           const MigrationSafetyResult *result)
{
    FILE *file;
    long position;

    if (manager->config.history_path == NULL)
        return true;
    file = fopen(manager->config.history_path, "a+");
    if (file == NULL)
        return false;
    if (fseek(file, 0, SEEK_END) != 0 || (position = ftell(file)) < 0) {
        fclose(file);
        return false;
    }
    if (position == 0)
        fputs("attempt_id,app_id,pid,start_time_ticks,action,source_node,target_node,state,validation,recovery,execution_result,execution_ms,consecutive_failures,cooldown,quarantined,detail\n", file);
    fprintf(file, "%s,%s,%ld,%llu,%d,%d,%d,%s,%d,%d,%s,%llu,%u,%s,%s,%s\n",
            result->attempt_id, request->app_id, (long)request->pid,
            (unsigned long long)request->start_time_ticks, request->action,
            request->source_numa_node, request->destination_numa_node,
            migration_safety_state_name(result->state), result->validation, result->recovery,
            MigrationResultName(result->execution_result),
            (unsigned long long)result->execution_time_ms, result->consecutive_failures,
            result->cooldown_entered ? "true" : "false", result->quarantined ? "true" : "false",
            result->detail);
    return fclose(file) == 0;
}

static void finish(MigrationSafetyManager *manager, app_safety_state_t *state,
                   const MigrationSafetyRequest *request, MigrationSafetyResult *result,
                   MigrationSafetyState terminal, const char *detail)
{
    FeedbackEvent event;
    FeedbackResult feedback_result;
    uint64_t current = now_ms();

    result->state = terminal;
    snprintf(result->detail, sizeof(result->detail), "%s", detail);
    if (terminal == MIGRATION_SAFETY_COMMITTED) {
        state->successes++;
        state->consecutive_failures = 0;
    } else if (terminal == MIGRATION_SAFETY_REJECTED) {
        state->rejections++;
    } else if (is_failure(terminal)) {
        state->failures++;
        state->consecutive_failures++;
        state->last_failure_ms = current;
    }
    if (terminal == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED) {
        state->rollbacks++;
        state->failures++;
        state->consecutive_failures++;
        state->last_failure_ms = current;
    }
    if (terminal == MIGRATION_SAFETY_ROLLBACK_FAILED) {
        state->rollbacks++;
        state->rollback_failures++;
        state->failures++;
        state->consecutive_failures++;
        state->last_failure_ms = current;
    }
    if (terminal == MIGRATION_SAFETY_TIMEOUT)
        state->timeouts++;
    if (state->consecutive_failures > manager->config.failure_limit) {
        state->quarantined = true;
        state->quarantines++;
        result->state = MIGRATION_SAFETY_QUARANTINED;
        result->quarantined = true;
    } else if (state->consecutive_failures == manager->config.failure_limit) {
        state->cooldown_until_ms = current + manager->config.cooldown_ms;
        result->state = MIGRATION_SAFETY_COOLDOWN;
        result->cooldown_entered = true;
    }
    result->consecutive_failures = state->consecutive_failures;
    memset(&event, 0, sizeof(event));
    snprintf(event.feedback_id, sizeof(event.feedback_id), "feedback-%.*s",
             (int)(sizeof(event.feedback_id) - sizeof("feedback-")), result->attempt_id);
    snprintf(event.migration_id, sizeof(event.migration_id), "%s", result->attempt_id);
    snprintf(event.app_id, sizeof(event.app_id), "%s", request->app_id);
    snprintf(event.entity_id, sizeof(event.entity_id), "%s", request->migration_request.phase5_decision.entity_id[0] != '\0' ? request->migration_request.phase5_decision.entity_id : "migration");
    event.pid = request->pid;
    event.start_time_ticks = request->start_time_ticks;
    event.start_time_ticks_available = true;
    event.action = request->action;
    snprintf(event.phase6_validation, sizeof(event.phase6_validation), "%s",
             request->migration_request.phase6_validation.final_decision);
    snprintf(event.migration_result, sizeof(event.migration_result), "%s",
             MigrationResultName(result->execution_result));
    event.event_kind = FEEDBACK_EVENT_MIGRATION_TERMINAL;
    event.terminal_outcome = feedback_outcome(result);
    snprintf(event.target_selection_reason, sizeof(event.target_selection_reason), "%s",
             result->target_reason);
    event.structural_validation_known = result->structural_validation_known;
    event.structural_validation_succeeded = result->structural_validation_succeeded;
    event.progress_known = result->progress_known;
    event.progress_observed = result->progress_observed;
    event.benefit_known = result->benefit_known;
    event.benefit_classification_available = true;
    event.benefit_classification = result->benefit_result;
    snprintf(event.benefit_reason, sizeof(event.benefit_reason), "%s", result->benefit_reason);
    snprintf(event.phase5_evidence_provenance, sizeof(event.phase5_evidence_provenance), "%s",
             request->migration_request.phase5_decision.phase5_evidence_provenance);
    event.phase5_runtime_generation = request->migration_request.phase5_decision.phase5_runtime_generation;
    event.phase5_runtime_generation_available =
        request->migration_request.phase5_decision.phase5_runtime_generation_available;
    snprintf(event.phase5_migration_id, sizeof(event.phase5_migration_id), "%s",
             request->migration_request.phase5_decision.migration_id);
    snprintf(event.phase6_migration_id, sizeof(event.phase6_migration_id), "%s",
             request->migration_request.phase6_validation.migration_id);
    event.page_checkpoint_known = request->action == VALIDATION_ACTION_MOVE_MEMORY;
    event.page_checkpoint_complete = result->page_checkpoint_complete;
    event.requested_page_count = result->page_checkpoint_requested_count;
    event.known_page_count = result->page_checkpoint_known_count;
    event.unknown_page_count = result->page_checkpoint_unknown_count;
    snprintf(event.page_checkpoint_result, sizeof(event.page_checkpoint_result), "%s",
              page_checkpoint_result_name(result->page_checkpoint_result));
    event.page_rollback_known = result->page_rollback_known;
    event.page_rollback_attempted = result->page_rollback_summary.attempted;
    snprintf(event.page_rollback_result, sizeof(event.page_rollback_result), "%s",
             result->page_rollback_known ?
             page_rollback_result_name(result->page_rollback_summary.result) : "UNAVAILABLE");
    event.page_rollback_requested_count = result->page_rollback_summary.requested_count;
    event.page_rollback_restored_count = result->page_rollback_summary.restored_count;
    event.page_rollback_failed_count = result->page_rollback_summary.failed_count;
    event.page_rollback_verified_count = result->page_rollback_summary.verified_count;
    snprintf(event.terminal_reason, sizeof(event.terminal_reason), "%.*s",
             (int)(sizeof(event.terminal_reason) - 1), result->detail);
    if (manager->config.feedback_fn != NULL)
        result->feedback_recorded = manager->config.feedback_fn(manager->config.callback_context, &event,
                                                                 &feedback_result);
    else if (ProcessFeedback(&event, &feedback_result) == FEEDBACK_UPDATE_TERMINAL_RECORDED)
        result->feedback_recorded = true;
    result->persistence_recorded = append_history(manager, request, result);
    monitor_profile_counter("phase7", "migration_safety_terminal", request->app_id, request->pid,
                            state->sequence, 1, migration_safety_state_name(result->state));
}

void migration_safety_config_default(MigrationSafetyConfig *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->execution_timeout_ms = 1000;
    config->validation_window_ms = 500;
    config->cooldown_ms = 30000;
    config->suppression_window_ms = 30000;
    config->failure_limit = 3;
    config->max_applications = 256;
}

const char *migration_safety_state_name(MigrationSafetyState state)
{
    static const char *names[] = {"ACTIVE", "PREPARING", "MIGRATING", "VALIDATING", "ROLLING_BACK", "COMMITTED", "MIGRATION_REJECTED", "EXECUTION_FAILED", "MIGRATION_TIMEOUT", "ROLLBACK_SUCCEEDED", "ROLLBACK_FAILED", "COOLDOWN", "QUARANTINED", "TARGET_GONE", "VALIDATION_UNKNOWN", "EXECUTION_DISABLED", "ACTION_TEMPORARILY_SUPPRESSED", "TARGET_UNAVAILABLE", "BENEFIT_REJECTED"};
    return state >= MIGRATION_SAFETY_ACTIVE && state <= MIGRATION_SAFETY_TARGET_UNAVAILABLE ? names[state] : "UNKNOWN";
}

MigrationSafetyManager *migration_safety_manager_create(void)
{
    return calloc(1, sizeof(MigrationSafetyManager));
}

bool migration_safety_manager_init(MigrationSafetyManager *manager, const MigrationSafetyConfig *config)
{
    MigrationSafetyConfig defaults;

    if (manager == NULL)
        return false;
    if (config == NULL) {
        migration_safety_config_default(&defaults);
        config = &defaults;
    }
    if (config->max_applications == 0 || config->failure_limit == 0 ||
        config->execution_timeout_ms == 0 || config->validation_window_ms == 0)
        return false;
    memset(manager, 0, sizeof(*manager));
    manager->config = *config;
    manager->applications = calloc(config->max_applications, sizeof(*manager->applications));
    return manager->applications != NULL;
}

bool migration_safety_manager_attempt(MigrationSafetyManager *manager,
                                      const MigrationSafetyRequest *request,
                                      MigrationSafetyResult *result)
{
    app_safety_state_t *state;
    MigrationReport report;
    uint64_t started;
    MigrationSafetyRequest prepared;
    MigrationTarget target;
    MigrationPageCheckpoint page_checkpoint;
    PageRollbackSummary page_rollback_summary;

    if (result == NULL)
        return false;
    memset(result, 0, sizeof(*result));
    result->state = MIGRATION_SAFETY_REJECTED;
    result->validation = MIGRATION_SAFETY_INSUFFICIENT_VALIDATION_DATA;
    result->recovery = MIGRATION_SAFETY_ROLLBACK_NOT_REQUIRED;
    result->execution_result = MIGRATION_INSUFFICIENT_INFORMATION;
    result->target_result = MIGRATION_TARGET_UNAVAILABLE;
    result->benefit_result = INSUFFICIENT_BENEFIT_EVIDENCE;
    result->page_checkpoint_result = PAGE_ADDRESS_SET_UNAVAILABLE;
    if (manager == NULL || request == NULL || !manager->config.enabled || request->app_id[0] == '\0')
        return false;
    state = app_state(manager, request);
    if (state == NULL)
        return false;
    snprintf(result->attempt_id, sizeof(result->attempt_id), "%.*s-%llu",
             (int)(sizeof(result->attempt_id) - 22), request->app_id,
             (unsigned long long)++state->sequence);
    state->attempts++;
    if (!identity_matches(manager, request->pid, request->start_time_ticks)) {
        result->execution_result = MIGRATION_TARGET_GONE;
        finish(manager, state, request, result, MIGRATION_SAFETY_TARGET_GONE, "PID identity changed or disappeared");
        return true;
    }
    if (state->quarantined) {
        finish(manager, state, request, result, MIGRATION_SAFETY_QUARANTINED, "application is quarantined");
        return true;
    }
    if (now_ms() < state->cooldown_until_ms) {
        finish(manager, state, request, result, MIGRATION_SAFETY_COOLDOWN, "application cooldown is active");
        return true;
    }
    if (request->action == VALIDATION_ACTION_MOVE_MEMORY && manager->config.page_checkpoint_fn != NULL) {
        memset(&page_checkpoint, 0, sizeof(page_checkpoint));
        result->page_checkpoint_result = manager->config.page_checkpoint_fn(
            manager->config.callback_context, request, result->attempt_id, &page_checkpoint);
        result->page_checkpoint_requested_count = page_checkpoint.requested_count;
        result->page_checkpoint_known_count = page_checkpoint.known_count;
        result->page_checkpoint_unknown_count = page_checkpoint.unknown_count;
        result->page_checkpoint_complete = page_checkpoint.complete;
        if (result->page_checkpoint_result != PAGE_CHECKPOINT_COMPLETE) {
            snprintf(result->target_reason, sizeof(result->target_reason), "%s",
                     page_checkpoint_result_name(result->page_checkpoint_result));
            page_checkpoint_release(&page_checkpoint);
            finish(manager, state, request, result, MIGRATION_SAFETY_TARGET_UNAVAILABLE,
                   result->target_reason);
            return true;
        }
        /* Page rollback is intentionally unavailable; do not make memory execution reachable. */
        result->target_result = MIGRATION_TARGET_PAGE_RECOVERY_UNAVAILABLE;
        snprintf(result->target_reason, sizeof(result->target_reason),
                 "PAGE_ROLLBACK_UNAVAILABLE checkpoint_complete=true requested_pages=%zu",
                 result->page_checkpoint_requested_count);
        page_checkpoint_release(&page_checkpoint);
        finish(manager, state, request, result, MIGRATION_SAFETY_TARGET_UNAVAILABLE,
               result->target_reason);
        return true;
    }
    if (manager->config.target_fn != NULL) {
        prepared = *request;
        prepared.target_policy_migration_intent_approved =
            (request->action == VALIDATION_ACTION_MOVE_THREAD ||
             request->action == VALIDATION_ACTION_MOVE_MEMORY) &&
            request->migration_request.phase5_decision.action == request->action &&
            strcmp(request->migration_request.phase6_validation.final_decision, "APPROVED") == 0;
        prepared.target_policy_safety_state_available = true;
        prepared.target_policy_cooldown_active = false;
        prepared.target_policy_quarantined = false;
        prepared.target_policy_history_available = true;
        prepared.target_policy_recent_equivalent_failure = state->last_failure_ms > 0 &&
            now_ms() - state->last_failure_ms < manager->config.suppression_window_ms;
        prepared.target_policy_previous_action = state->last_action;
        prepared.target_policy_previous_source_node = state->last_source;
        prepared.target_policy_previous_target_node = state->last_target;
        memset(&target, 0, sizeof(target));
        target.target_numa_node = -1;
        target.source_numa_node = -1;
        result->target_result = manager->config.target_fn(manager->config.callback_context, &prepared,
                                                           result->attempt_id, &target);
        snprintf(result->target_reason, sizeof(result->target_reason), "%s", target.reason);
        if (result->target_result != MIGRATION_TARGET_AVAILABLE) {
            snprintf(result->detail, sizeof(result->detail), "%.*s",
                     (int)(sizeof(result->detail) - 1), target.reason[0] != '\0' ?
                     target.reason : migration_target_result_name(result->target_result));
            finish(manager, state, request, result, MIGRATION_SAFETY_TARGET_UNAVAILABLE, result->detail);
            return true;
        }
        if (target.pid != request->pid || target.start_time_ticks != request->start_time_ticks ||
            strcmp(target.attempt_id, result->attempt_id) != 0 || target.action != request->action) {
            result->target_result = MIGRATION_TARGET_STALE;
            snprintf(result->target_reason, sizeof(result->target_reason), "TARGET_STALE");
            finish(manager, state, request, result, MIGRATION_SAFETY_TARGET_UNAVAILABLE,
                    "TARGET_STALE");
            return true;
        }
        prepared = *request;
        prepared.target_valid = true;
        prepared.target_policy_safety_state_available = true;
        prepared.target_policy_cooldown_active = false;
        prepared.target_policy_quarantined = false;
        prepared.target_policy_history_available = true;
        prepared.target_policy_recent_equivalent_failure = state->last_failure_ms > 0 &&
            now_ms() - state->last_failure_ms < manager->config.suppression_window_ms;
        prepared.target_policy_previous_action = state->last_action;
        prepared.target_policy_previous_source_node = state->last_source;
        prepared.target_policy_previous_target_node = state->last_target;
        if (target.source_node_known) {
            prepared.source_numa_node = target.source_numa_node;
            prepared.migration_request.source_numa_node = target.source_numa_node;
            prepared.migration_request.numa_nodes_available = true;
        }
        if (target.has_target_numa_node) {
            prepared.destination_numa_node = target.target_numa_node;
            prepared.migration_request.destination_numa_node = target.target_numa_node;
            prepared.migration_request.numa_nodes_available = true;
        }
        if (target.has_target_cpu_mask) {
            prepared.migration_request.requested_cpu_set = target.target_cpu_mask;
            prepared.migration_request.requested_cpu_set_available = true;
        }
        if (target.source_node_known)
            prepared.placement_available = target.has_target_numa_node;
        request = &prepared;
    }
    if (state->last_action == request->action && state->last_source == request->source_numa_node &&
        state->last_target == request->destination_numa_node && state->last_failure_ms > 0 &&
        now_ms() - state->last_failure_ms < manager->config.suppression_window_ms) {
        finish(manager, state, request, result, MIGRATION_SAFETY_SUPPRESSED, "recent equivalent migration failed");
        return true;
    }
    if ((request->action != VALIDATION_ACTION_MOVE_MEMORY && request->action != VALIDATION_ACTION_MOVE_THREAD) ||
        request->migration_request.pid != request->pid ||
        request->migration_request.start_time_ticks != request->start_time_ticks ||
        !request->migration_request.start_time_ticks_available ||
        request->migration_request.phase5_decision.action != request->action ||
        strcmp(request->migration_request.phase6_validation.final_decision, "APPROVED") != 0 ||
        !request->placement_available || !request->target_valid || !request->system_safe ||
        (request->action == VALIDATION_ACTION_MOVE_MEMORY &&
         request->source_numa_node == request->destination_numa_node)) {
        finish(manager, state, request, result, MIGRATION_SAFETY_REJECTED, "placement, target, system, or identity evidence is unavailable");
        return true;
    }
    if (manager->config.benefit_fn != NULL) {
        BenefitDecision benefit;

        /* Phase 5/6 records predate the attempt; bind their validated bundle now. */
        prepared = *request;
        prepared.migration_request.benefit_evidence_bound = true;
        snprintf(prepared.migration_request.benefit_evidence_attempt_id,
                 sizeof(prepared.migration_request.benefit_evidence_attempt_id), "%s",
                 result->attempt_id);
        request = &prepared;
        memset(&benefit, 0, sizeof(benefit));
        result->benefit_result = manager->config.benefit_fn(manager->config.callback_context, request,
                                                             &target, result->attempt_id, &benefit);
        snprintf(result->benefit_reason, sizeof(result->benefit_reason), "%s",
                 benefit.reason[0] != '\0' ? benefit.reason :
                 benefit_classification_name(result->benefit_result));
        if (result->benefit_result != BENEFIT_SUPPORTED) {
            finish(manager, state, request, result, MIGRATION_SAFETY_BENEFIT_REJECTED,
                   benefit.detail[0] != '\0' ? benefit.detail : result->benefit_reason);
            return true;
        }
    }
    state->last_action = request->action;
    state->last_source = request->source_numa_node;
    state->last_target = request->destination_numa_node;
    if (!manager->config.execution_enabled || manager->config.execute_fn == NULL) {
        finish(manager, state, request, result, MIGRATION_SAFETY_EXECUTION_DISABLED, "migration execution is disabled");
        return true;
    }
    memset(&report, 0, sizeof(report));
    /* Capture only for attempts that have passed every pre-execution terminal gate. */
    if (manager->config.capture_before_fn != NULL &&
        !manager->config.capture_before_fn(manager->config.callback_context, request,
                                           result->attempt_id)) {
        finish(manager, state, request, result, MIGRATION_SAFETY_VALIDATION_UNKNOWN,
               "before-execution structural validation snapshot is unavailable");
        return true;
    }
    started = now_ms();
    result->execution_result = manager->config.execute_fn(manager->config.callback_context,
                                                            &request->migration_request, &report);
    result->execution_time_ms = now_ms() - started;
    if (result->execution_time_ms > manager->config.execution_timeout_ms) {
        if ((result->execution_result == MIGRATION_SUCCESS || result->execution_result == MIGRATION_PARTIAL_SUCCESS) &&
            identity_matches(manager, request->pid, request->start_time_ticks) &&
            manager->config.rollback_fn != NULL)
            result->recovery = manager->config.rollback_fn(manager->config.callback_context, request, &report);
        finish(manager, state, request, result, MIGRATION_SAFETY_TIMEOUT, "migration exceeded controller timeout");
        return true;
    }
    if (result->execution_result != MIGRATION_SUCCESS && result->execution_result != MIGRATION_PARTIAL_SUCCESS) {
        finish(manager, state, request, result, MIGRATION_SAFETY_EXECUTION_FAILED, "migration execution failed safely");
        return true;
    }
    if (!identity_matches(manager, request->pid, request->start_time_ticks)) {
        result->execution_result = MIGRATION_TARGET_GONE;
        finish(manager, state, request, result, MIGRATION_SAFETY_TARGET_GONE,
               "PID identity changed after migration execution");
        return true;
    }
    result->validation = manager->config.validate_fn == NULL ?
        MIGRATION_SAFETY_INSUFFICIENT_VALIDATION_DATA :
        manager->config.validate_fn(manager->config.callback_context, request, result->attempt_id,
                                    &report);
    result->structural_validation_known =
        result->validation == MIGRATION_SAFETY_STRUCTURALLY_VALID ||
        result->validation == MIGRATION_SAFETY_STRUCTURALLY_VALID_PROGRESS ||
        result->validation == MIGRATION_SAFETY_NO_PROGRESS_INCONCLUSIVE ||
        result->validation == MIGRATION_SAFETY_STRUCTURAL_MISMATCH;
    result->structural_validation_succeeded =
        result->validation == MIGRATION_SAFETY_STRUCTURALLY_VALID ||
        result->validation == MIGRATION_SAFETY_STRUCTURALLY_VALID_PROGRESS ||
        result->validation == MIGRATION_SAFETY_NO_PROGRESS_INCONCLUSIVE;
    result->progress_known = result->validation == MIGRATION_SAFETY_STRUCTURALLY_VALID_PROGRESS ||
        result->validation == MIGRATION_SAFETY_NO_PROGRESS_INCONCLUSIVE;
    result->progress_observed = result->validation == MIGRATION_SAFETY_STRUCTURALLY_VALID_PROGRESS;
    result->benefit_known = false;
    if (result->validation == MIGRATION_SAFETY_STRUCTURALLY_VALID ||
        result->validation == MIGRATION_SAFETY_STRUCTURALLY_VALID_PROGRESS ||
        result->validation == MIGRATION_SAFETY_HEALTHY ||
        result->validation == MIGRATION_SAFETY_NO_MEANINGFUL_CHANGE) {
        finish(manager, state, request, result, MIGRATION_SAFETY_COMMITTED,
               "post-migration structural verification is acceptable");
        return true;
    }
    if (result->validation == MIGRATION_SAFETY_VALIDATION_TARGET_GONE) {
        finish(manager, state, request, result, MIGRATION_SAFETY_TARGET_GONE, "target disappeared during validation");
        return true;
    }
    if (result->validation == MIGRATION_SAFETY_INSUFFICIENT_VALIDATION_DATA) {
        finish(manager, state, request, result, MIGRATION_SAFETY_VALIDATION_UNKNOWN, "comparable post-migration metrics are unavailable");
        return true;
    }
    if (result->validation == MIGRATION_SAFETY_NO_PROGRESS_INCONCLUSIVE) {
        finish(manager, state, request, result, MIGRATION_SAFETY_VALIDATION_UNKNOWN,
               "no CPU-time progress was observed; idle or blocked state is inconclusive");
        return true;
    }
    if (!identity_matches(manager, request->pid, request->start_time_ticks)) {
        result->execution_result = MIGRATION_TARGET_GONE;
        finish(manager, state, request, result, MIGRATION_SAFETY_TARGET_GONE,
               "PID identity changed before rollback");
        return true;
    }
    prepared = *request;
    memset(&page_rollback_summary, 0, sizeof(page_rollback_summary));
    page_rollback_summary.result = PAGE_ROLLBACK_UNAVAILABLE;
    prepared.page_rollback_summary = &page_rollback_summary;
    request = &prepared;
    result->recovery = manager->config.rollback_fn == NULL ? MIGRATION_SAFETY_ROLLBACK_UNAVAILABLE :
        manager->config.rollback_fn(manager->config.callback_context, request, &report);
    if (request->action == VALIDATION_ACTION_MOVE_MEMORY) {
        result->page_rollback_known = true;
        result->page_rollback_summary = page_rollback_summary;
    }
    if (result->recovery == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT)
        finish(manager, state, request, result, MIGRATION_SAFETY_ROLLBACK_SUCCEEDED,
               result->validation == MIGRATION_SAFETY_STRUCTURAL_MISMATCH ?
               "post-migration structural verification required rollback" :
               "post-migration health required rollback");
    else
        finish(manager, state, request, result, MIGRATION_SAFETY_ROLLBACK_FAILED,
                result->validation == MIGRATION_SAFETY_STRUCTURAL_MISMATCH ?
                "post-migration structural verification failed; rollback unavailable or failed" :
                result->validation == MIGRATION_SAFETY_SUSPECTED_STALL ?
                "suspected migration-associated stall; rollback unavailable or failed" :
                "post-migration degradation; rollback unavailable or failed");
    return true;
}

void migration_safety_manager_shutdown(MigrationSafetyManager *manager)
{
    if (manager == NULL)
        return;
    free(manager->applications);
    memset(manager, 0, sizeof(*manager));
}

void migration_safety_manager_destroy(MigrationSafetyManager *manager)
{
    if (manager == NULL)
        return;
    migration_safety_manager_shutdown(manager);
    free(manager);
}

#ifdef AWAVMA_RUNTIME_TESTING
bool migration_safety_test_resume_page_recovery(MigrationSafetyManager *manager,
                                                 const MigrationSafetyRequest *request,
                                                 const char *active_attempt_id,
                                                 const MigrationPageCheckpoint *checkpoint,
                                                 MigrationSafetyResult *result)
{
    app_safety_state_t *state;
    MigrationSafetyRequest prepared;
    MigrationReport report;

    if (manager == NULL || request == NULL || checkpoint == NULL || result == NULL ||
        active_attempt_id == NULL || active_attempt_id[0] == '\0' ||
        request->action != VALIDATION_ACTION_MOVE_MEMORY)
        return false;
    state = app_state(manager, request);
    if (state == NULL)
        return false;
    memset(result, 0, sizeof(*result));
    snprintf(result->attempt_id, sizeof(result->attempt_id), "%s", active_attempt_id);
    result->execution_result = MIGRATION_SYSTEM_ERROR;
    result->validation = MIGRATION_SAFETY_STRUCTURAL_MISMATCH;
    result->page_checkpoint_result = PAGE_CHECKPOINT_COMPLETE;
    result->page_checkpoint_complete = true;
    result->page_checkpoint_requested_count = checkpoint->requested_count;
    result->page_checkpoint_known_count = checkpoint->known_count;
    prepared = *request;
    snprintf(prepared.migration_request.benefit_evidence_attempt_id,
             sizeof(prepared.migration_request.benefit_evidence_attempt_id), "%s", active_attempt_id);
    memset(&report, 0, sizeof(report));
    prepared.page_rollback_summary = &result->page_rollback_summary;
    result->page_rollback_summary.result = PAGE_ROLLBACK_UNAVAILABLE;
    result->page_rollback_known = true;
    result->page_rollback_summary.requested_count = checkpoint->requested_count;
    if (strcmp(active_attempt_id, checkpoint->attempt_id) != 0) {
        result->page_rollback_summary.result = PAGE_ROLLBACK_ATTEMPT_MISMATCH;
        finish(manager, state, &prepared, result, MIGRATION_SAFETY_ROLLBACK_FAILED,
               "test-only page checkpoint attempt mismatch");
        return true;
    }
    if (!checkpoint->complete) {
        result->page_rollback_summary.result = PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE;
        finish(manager, state, &prepared, result, MIGRATION_SAFETY_ROLLBACK_FAILED,
               "test-only page checkpoint incomplete");
        return true;
    }
    if (checkpoint->pid != request->pid || checkpoint->start_time_ticks != request->start_time_ticks ||
        manager->config.rollback_fn == NULL || !identity_matches(manager, request->pid, request->start_time_ticks))
        return false;
    result->recovery = manager->config.rollback_fn(manager->config.callback_context, &prepared, &report);
    state->last_action = request->action;
    state->last_source = request->source_numa_node;
    state->last_target = request->destination_numa_node;
    finish(manager, state, &prepared, result,
           result->recovery == MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT ?
           MIGRATION_SAFETY_ROLLBACK_SUCCEEDED : MIGRATION_SAFETY_ROLLBACK_FAILED,
           "test-only post-checkpoint recovery required");
    return true;
}
#endif
