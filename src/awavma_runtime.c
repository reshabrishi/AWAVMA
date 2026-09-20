#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"

#include "application_manager.h"
#include "classifier.h"
#include "migration_safety_manager.h"
#include "migration_validation_snapshot.h"
#include "monitor_profile.h"
#include "page_checkpoint.h"
#include "page_rollback.h"
#include "runtime_migration_metadata.h"
#include "thread_target_policy.h"
#include "worker_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ROOT_DIR "results/runtime"
#define DEFAULT_BIN_DIR "bin"
#define DEFAULT_PHASE_CONFIG "config/awavma.conf"
#define DEFAULT_MONITOR_INTERVAL_MS 100U
#define DEFAULT_EVALUATION_INTERVAL_MS 1000U
#define DEFAULT_MAX_APPLICATIONS 256U
#define DEFAULT_WORKERS 2U
#define DEFAULT_QUEUE_CAPACITY 64U

static size_t split_csv(char *line, char **fields, size_t capacity);
static int column_index(char **fields, size_t count, const char *name);
static int path_join(char *buffer, size_t size, const char *left, const char *right);
static int make_directory(const char *path);
static bool safe_app_id(const char *app_id);

struct awavma_runtime {
    awavma_runtime_config_t config;
    application_manager_t *manager;
    worker_pool_t *pool;
    runtime_monitor_t *monitor;
    awavma_runtime_record_t *records;
    size_t record_count;
    char manager_log_path[4096];
    char manager_results_path[4096];
    char manager_table_path[4096];
    char monitor_dir[4096];
    char monitor_results_path[4096];
    char monitor_log_path[4096];
    char runtime_results_path[4096];
    char migration_safety_history_path[4096];
    MigrationSafetyManager *migration_safety;
    MigrationValidationSnapshot validation_before;
    bool validation_before_available;
    RuntimeMigrationCheckpoint rollback_checkpoint;
    bool rollback_checkpoint_available;
    char rollback_attempt_id[128];
    MigrationPageCheckpoint page_rollback_checkpoint;
    bool page_rollback_checkpoint_available;
    bool migration_initialized;
#ifdef AWAVMA_RUNTIME_TESTING
    awavma_runtime_test_target_case_t test_target_case;
    awavma_runtime_test_target_stats_t test_target_stats;
#endif
    bool initialized;
    _Atomic bool stop_requested;
};

static void release_migration_checkpoint(awavma_runtime_t *runtime, const char *attempt_id)
{
    if (runtime == NULL || !runtime->rollback_checkpoint_available || attempt_id == NULL ||
        strcmp(runtime->rollback_attempt_id, attempt_id) != 0)
        return;
    runtime_migration_checkpoint_release(&runtime->rollback_checkpoint);
    runtime->rollback_checkpoint_available = false;
    runtime->rollback_attempt_id[0] = '\0';
}

static void release_page_rollback_checkpoint(awavma_runtime_t *runtime, const char *attempt_id)
{
    if (runtime == NULL || !runtime->page_rollback_checkpoint_available || attempt_id == NULL ||
        strcmp(runtime->page_rollback_checkpoint.attempt_id, attempt_id) != 0)
        return;
    page_checkpoint_release(&runtime->page_rollback_checkpoint);
    runtime->page_rollback_checkpoint_available = false;
}

static bool runtime_identity_matches(void *context, pid_t pid, uint64_t start_time_ticks)
{
    awavma_runtime_t *runtime = context;
    RuntimeMigrationMetadata metadata;

    if (!runtime_get_migration_metadata(pid, start_time_ticks, &metadata) ||
        !metadata.process_exists || !metadata.identity_match)
        return false;
#ifdef AWAVMA_RUNTIME_TESTING
    if (runtime != NULL &&
        runtime->test_target_case == AWAVMA_RUNTIME_TEST_TARGET_IDENTITY_MISMATCH_AFTER_EXEC &&
        runtime->test_target_stats.executor_calls > 0)
        return false;
#else
    (void)runtime;
#endif
    return true;
}

static MigrationResultCode runtime_execute_migration(void *context, const MigrationRequest *request,
                                                      MigrationReport *report)
{
    awavma_runtime_t *runtime = context;
    MigrationResultCode result;

    if (runtime == NULL || !runtime->migration_initialized)
        return MIGRATION_SYSTEM_ERROR;
#ifdef AWAVMA_RUNTIME_TESTING
    runtime->test_target_stats.executor_calls++;
#endif
    result = Migration_Execute(request, report);
#ifdef AWAVMA_RUNTIME_TESTING
    if (result == MIGRATION_SUCCESS &&
        runtime->test_target_case == AWAVMA_RUNTIME_TEST_TARGET_PLACEMENT_MISMATCH &&
        runtime->rollback_checkpoint_available) {
        cpu_set_t mismatched;

        CPU_ZERO(&mismatched);
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
            if (CPU_ISSET(cpu, &runtime->rollback_checkpoint.original_affinity) &&
                !CPU_ISSET(cpu, &request->requested_cpu_set)) {
                CPU_SET(cpu, &mismatched);
                break;
            }
        if (CPU_COUNT(&mismatched) > 0)
            (void)sched_setaffinity(request->pid, sizeof(mismatched), &mismatched);
    }
    /* Keep the active-child snapshot window long enough to cross a CPU accounting tick. */
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 30000000}, NULL);
#endif
    return result;
}

static MigrationSafetyValidation runtime_validate_migration(void *context,
                                                              const MigrationSafetyRequest *request,
                                                              const char *attempt_id,
                                                              const MigrationReport *report)
{
    awavma_runtime_t *runtime = context;
    MigrationValidationSnapshot after;
    MigrationValidationOutcome outcome;

    (void)report;
    if (runtime == NULL || request == NULL || attempt_id == NULL)
        return MIGRATION_SAFETY_INSUFFICIENT_VALIDATION_DATA;
#ifdef AWAVMA_RUNTIME_TESTING
    runtime->test_target_stats.validation_after_calls++;
#endif
    if (!runtime->validation_before_available ||
        strcmp(runtime->validation_before.attempt_id, attempt_id) != 0 ||
        !migration_validation_snapshot_collect(request->pid, request->start_time_ticks, attempt_id,
                                               &after)) {
        runtime->validation_before_available = false;
        return MIGRATION_SAFETY_INSUFFICIENT_VALIDATION_DATA;
    }
    outcome = migration_validation_compare(&runtime->validation_before, &after,
                                           &request->migration_request.requested_cpu_set,
                                           request->migration_request.requested_cpu_set_available);
#ifdef AWAVMA_RUNTIME_TESTING
    runtime->test_target_stats.before_cpu_time_available =
        runtime->validation_before.process_cpu_time_available;
    runtime->test_target_stats.before_cpu_time_ticks =
        runtime->validation_before.process_cpu_time_ticks;
    runtime->test_target_stats.after_cpu_time_available = after.process_cpu_time_available;
    runtime->test_target_stats.after_cpu_time_ticks = after.process_cpu_time_ticks;
#endif
    runtime->validation_before_available = false;
    switch (outcome) {
    case MIGRATION_VALIDATION_STRUCTURALLY_VALID_PROGRESS:
        return MIGRATION_SAFETY_STRUCTURALLY_VALID_PROGRESS;
    case MIGRATION_VALIDATION_NO_PROGRESS_INCONCLUSIVE:
        return MIGRATION_SAFETY_NO_PROGRESS_INCONCLUSIVE;
    case MIGRATION_VALIDATION_STRUCTURALLY_VALID:
        return MIGRATION_SAFETY_STRUCTURALLY_VALID;
    case MIGRATION_VALIDATION_PLACEMENT_NOT_APPLIED:
        return MIGRATION_SAFETY_STRUCTURAL_MISMATCH;
    case MIGRATION_VALIDATION_TARGET_GONE:
    case MIGRATION_VALIDATION_IDENTITY_CHANGED:
        return MIGRATION_SAFETY_VALIDATION_TARGET_GONE;
    case MIGRATION_VALIDATION_INSUFFICIENT_DATA:
    default:
        return MIGRATION_SAFETY_INSUFFICIENT_VALIDATION_DATA;
    }
}

static bool runtime_capture_before_migration(void *context,
                                             const MigrationSafetyRequest *request,
                                             const char *attempt_id)
{
    awavma_runtime_t *runtime = context;
    RuntimeMigrationMetadata metadata;

    if (runtime == NULL || request == NULL || attempt_id == NULL)
        return false;
    runtime->validation_before_available = false;
    release_migration_checkpoint(runtime, runtime->rollback_attempt_id);
#ifdef AWAVMA_RUNTIME_TESTING
    runtime->test_target_stats.validation_before_calls++;
    if (runtime->test_target_case == AWAVMA_RUNTIME_TEST_TARGET_CAPTURE_FAILURE)
        return false;
#endif
    if (!migration_validation_snapshot_collect(request->pid, request->start_time_ticks, attempt_id,
                                               &runtime->validation_before))
        return false;
    /* Re-read the live checkpoint after the structural snapshot without inventing page state. */
    if (!runtime_get_migration_metadata(request->pid, request->start_time_ticks, &metadata) ||
        !runtime_migration_checkpoint_affinity(&metadata, &runtime->rollback_checkpoint))
        return false;
    runtime->rollback_checkpoint_available = true;
    snprintf(runtime->rollback_attempt_id, sizeof(runtime->rollback_attempt_id), "%s", attempt_id);
    runtime->validation_before_available = true;
    return true;
}

static MigrationSafetyRecovery runtime_rollback_migration(void *context,
                                                           const MigrationSafetyRequest *request,
                                                           const MigrationReport *report)
{
    awavma_runtime_t *runtime = context;
    RuntimeMigrationRollbackResult rollback_result;

    (void)report;
#ifdef AWAVMA_RUNTIME_TESTING
    if (context != NULL)
        ((awavma_runtime_t *)context)->test_target_stats.rollback_calls++;
#endif
    if (runtime == NULL || request == NULL)
        return MIGRATION_SAFETY_ROLLBACK_UNAVAILABLE;
    if (request->action == VALIDATION_ACTION_MOVE_MEMORY) {
        PageRollbackRequest rollback_request = {
            .checkpoint = &runtime->page_rollback_checkpoint, .pid = request->pid,
            .start_time_ticks = request->start_time_ticks,
            .attempt_id = request->migration_request.benefit_evidence_attempt_id
        };
        PageRollbackSummary summary;
        PageRollbackResult page_result;

        if (!runtime->page_rollback_checkpoint_available)
            return MIGRATION_SAFETY_ROLLBACK_UNAVAILABLE;
        page_result = page_rollback_restore(&rollback_request, &summary);
        if (request->page_rollback_summary != NULL)
            *request->page_rollback_summary = summary;
        return page_result == PAGE_ROLLBACK_COMPLETE ? MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT :
            page_result == PAGE_ROLLBACK_PARTIAL ? MIGRATION_SAFETY_ROLLBACK_PARTIAL :
            page_result == PAGE_ROLLBACK_UNAVAILABLE || page_result == PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE ?
            MIGRATION_SAFETY_ROLLBACK_UNAVAILABLE : MIGRATION_SAFETY_ROLLBACK_FAILED_RESULT;
    }
    if (request->action != VALIDATION_ACTION_MOVE_THREAD || !runtime->rollback_checkpoint_available)
        return MIGRATION_SAFETY_ROLLBACK_UNAVAILABLE;
    rollback_result = runtime_migration_restore_affinity(&runtime->rollback_checkpoint);
#ifdef AWAVMA_RUNTIME_TESTING
    runtime->test_target_stats.rollback_succeeded =
        rollback_result == RUNTIME_MIGRATION_ROLLBACK_COMPLETE;
#endif
    return rollback_result == RUNTIME_MIGRATION_ROLLBACK_COMPLETE ?
        MIGRATION_SAFETY_ROLLBACK_SUCCEEDED_RESULT :
        rollback_result == RUNTIME_MIGRATION_ROLLBACK_CHECKPOINT_UNAVAILABLE ?
        MIGRATION_SAFETY_ROLLBACK_UNAVAILABLE : MIGRATION_SAFETY_ROLLBACK_FAILED_RESULT;
}

static PageCheckpointResult runtime_capture_page_checkpoint(
    void *context, const MigrationSafetyRequest *request, const char *attempt_id,
    MigrationPageCheckpoint *checkpoint)
{
    PageCheckpointRequest checkpoint_request;

    awavma_runtime_t *runtime = context;
    if (request == NULL || checkpoint == NULL || request->action != VALIDATION_ACTION_MOVE_MEMORY)
        return PAGE_ADDRESS_SET_UNAVAILABLE;
    memset(&checkpoint_request, 0, sizeof(checkpoint_request));
    checkpoint_request.pid = request->pid;
    checkpoint_request.start_time_ticks = request->start_time_ticks;
    checkpoint_request.attempt_id = attempt_id;
    checkpoint_request.pages = (const void *const *)request->migration_request.pages;
    checkpoint_request.page_count = request->migration_request.page_count;
    /* Runtime has no page selector; metadata flags alone are not page-address provenance. */
    checkpoint_request.authoritative_address_set = request->migration_request.page_metadata_available &&
        request->migration_request.page_addresses_authoritative &&
        request->migration_request.memory_region_verified && checkpoint_request.pages != NULL;
    PageCheckpointResult result = page_checkpoint_capture(&checkpoint_request, checkpoint);
    if (result == PAGE_CHECKPOINT_COMPLETE && runtime != NULL) {
        MigrationPageCheckpoint copy = *checkpoint;

        copy.entries = calloc(checkpoint->requested_count, sizeof(*copy.entries));
        if (copy.entries == NULL)
            return PAGE_CHECKPOINT_ALLOCATION_FAILED;
        memcpy(copy.entries, checkpoint->entries, checkpoint->requested_count * sizeof(*copy.entries));
        release_page_rollback_checkpoint(runtime, runtime->page_rollback_checkpoint.attempt_id);
        runtime->page_rollback_checkpoint = copy;
        runtime->page_rollback_checkpoint_available = true;
    }
    return result;
}

static MigrationTargetResult runtime_get_migration_target(void *context,
                                                          const MigrationSafetyRequest *request,
                                                          const char *attempt_id,
                                                          MigrationTarget *target)
{
    RuntimeMigrationMetadata metadata;
    ThreadTargetPolicyInput policy_input;

    (void)context;
    if (request == NULL || target == NULL ||
        (request->action != VALIDATION_ACTION_MOVE_THREAD &&
         request->action != VALIDATION_ACTION_MOVE_MEMORY))
        return MIGRATION_TARGET_UNAVAILABLE;
#ifdef AWAVMA_RUNTIME_TESTING
    MigrationTargetInput input;

    memset(&input, 0, sizeof(input));
    input.pid = request->pid;
    input.start_time_ticks = request->start_time_ticks;
    input.attempt_id = attempt_id;
    input.action = request->action;
    input.source_node_available = request->placement_available && request->source_numa_node >= 0;
    input.source_numa_node = request->source_numa_node;
    input.requires_cross_node = request->action == VALIDATION_ACTION_MOVE_MEMORY ||
                                request->target_requires_cross_node;
    input.source = MIGRATION_TARGET_SOURCE_NONE;
    if (context != NULL) {
        awavma_runtime_t *runtime = context;

        runtime->test_target_stats.target_provider_calls++;
        if (runtime->test_target_case == AWAVMA_RUNTIME_TEST_TARGET_INVALID) {
            input.has_authoritative_cpu_mask = true;
            CPU_SET(CPU_SETSIZE - 1, &input.authoritative_cpu_mask);
        } else if (runtime->test_target_case == AWAVMA_RUNTIME_TEST_TARGET_VALID ||
                   runtime->test_target_case == AWAVMA_RUNTIME_TEST_TARGET_PLACEMENT_MISMATCH ||
                   runtime->test_target_case == AWAVMA_RUNTIME_TEST_TARGET_CAPTURE_FAILURE ||
                   runtime->test_target_case == AWAVMA_RUNTIME_TEST_TARGET_IDENTITY_MISMATCH_AFTER_EXEC) {
            RuntimeMigrationMetadata metadata;

            if (runtime_get_migration_metadata(request->pid, request->start_time_ticks, &metadata) &&
                metadata.identity_match && metadata.affinity_available) {
                for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
                    if (CPU_ISSET(cpu, &metadata.affinity)) {
                        input.has_authoritative_cpu_mask = true;
                        CPU_SET(cpu, &input.authoritative_cpu_mask);
                        input.source = MIGRATION_TARGET_SOURCE_TEST;
                        break;
                    }
            }
        }
    }
#endif
#ifdef AWAVMA_RUNTIME_TESTING
    /* Existing runtime migration tests inject only controlled provider inputs. */
    if (context != NULL &&
        ((awavma_runtime_t *)context)->test_target_case != AWAVMA_RUNTIME_TEST_TARGET_POLICY_LIVE) {
        MigrationTargetResult result = migration_target_provider_get(&input, NULL, target);

        if (result == MIGRATION_TARGET_AVAILABLE && request->placement_available) {
            target->source_node_known = request->source_numa_node >= 0;
            target->source_numa_node = request->source_numa_node;
            /* Test-only provider inputs use a synthetic cross-node relationship. */
            target->has_target_numa_node = true;
            target->target_numa_node = 1;
            target->candidate_count = 1;
        }
        return result;
    }
#endif
    if (!runtime_get_migration_metadata(request->pid, request->start_time_ticks, &metadata))
        return MIGRATION_TARGET_INTERNAL_ERROR;
    memset(&policy_input, 0, sizeof(policy_input));
    policy_input.pid = request->pid;
    policy_input.start_time_ticks = request->start_time_ticks;
    policy_input.attempt_id = attempt_id;
    policy_input.action = request->action;
    policy_input.migration_intent_approved = request->target_policy_migration_intent_approved;
    policy_input.identity_match = metadata.identity_match;
    policy_input.safety_state_available = request->target_policy_safety_state_available;
    policy_input.cooldown_active = request->target_policy_cooldown_active;
    policy_input.quarantined = request->target_policy_quarantined;
    policy_input.source_cpu_available = metadata.current_cpu_available;
    policy_input.source_cpu = metadata.current_cpu;
    policy_input.allowed_affinity_available = metadata.affinity_available;
    policy_input.allowed_affinity = metadata.affinity;
    policy_input.history_available = request->target_policy_history_available;
    policy_input.recent_equivalent_failure = request->target_policy_recent_equivalent_failure;
    policy_input.previous_action = request->target_policy_previous_action;
    policy_input.previous_source_node = request->target_policy_previous_source_node;
    policy_input.previous_target_node = request->target_policy_previous_target_node;
    return thread_target_policy_select(&policy_input, NULL, target);
}

static BenefitClassification runtime_classify_benefit(void *context,
                                                      const MigrationSafetyRequest *request,
                                                      const MigrationTarget *target,
                                                      const char *attempt_id,
                                                      BenefitDecision *decision)
{
    RuntimeMigrationMetadata metadata;
    MigrationTargetTopology topology;
    BenefitClassifierInput input;
    bool target_online = true;
    bool target_permitted = true;
    bool source_target_valid = false;

    if (request == NULL || target == NULL || attempt_id == NULL || decision == NULL ||
        !runtime_get_migration_metadata(request->pid, request->start_time_ticks, &metadata))
        return benefit_classifier_evaluate(NULL, decision);
    if (!migration_target_topology_read(&topology))
        target_online = false;
    for (int cpu = 0; target_online && cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &target->target_cpu_mask) &&
            (!CPU_ISSET(cpu, &topology.online_cpus) || topology.cpu_node[cpu] != target->target_numa_node))
            target_online = false;
    if (!metadata.affinity_available)
        target_permitted = false;
    for (int cpu = 0; target_permitted && cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &target->target_cpu_mask) && !CPU_ISSET(cpu, &metadata.affinity))
            target_permitted = false;
    source_target_valid = target->source_node_known && target->has_target_numa_node &&
                          target->source_numa_node != target->target_numa_node;
    memset(&input, 0, sizeof(input));
    input.pid = request->pid;
    input.start_time_ticks = request->start_time_ticks;
    input.attempt_id = attempt_id;
    input.action = request->action;
    input.process_active = metadata.process_exists;
    input.identity_match = metadata.identity_match;
    input.phase5_fresh = request->migration_request.phase5_decision.phase5_evidence_available &&
                          request->migration_request.phase5_decision.phase5_runtime_generation_available &&
                          request->migration_request.benefit_evidence_runtime_generation ==
                              request->migration_request.phase5_decision.phase5_runtime_generation &&
                          request->migration_request.phase5_decision.pid == (long)request->pid &&
                          request->migration_request.phase5_decision.action == request->action;
    input.phase6_fresh = request->migration_request.phase6_validation.pid == (long)request->pid &&
                          request->migration_request.phase6_validation.action == request->action &&
                          request->migration_request.phase5_decision.migration_id[0] != '\0' &&
                          strcmp(request->migration_request.phase5_decision.migration_id,
                                 request->migration_request.phase6_validation.migration_id) == 0;
    input.evidence_attempt_bound = request->migration_request.benefit_evidence_bound &&
                                   strcmp(request->migration_request.benefit_evidence_attempt_id,
                                          attempt_id) == 0;
    input.decision = &request->migration_request.phase5_decision;
    input.validation = &request->migration_request.phase6_validation;
    input.target = target;
    input.target_provider_validated = request->target_valid;
    input.target_online = target_online;
    input.target_permitted = target_permitted;
    input.source_known = target->source_node_known;
    input.source_target_valid = source_target_valid;
    input.cooldown_active = request->target_policy_cooldown_active;
    input.quarantined = request->target_policy_quarantined;
    input.history_suppressed = request->target_policy_recent_equivalent_failure;
    input.calibration.state = context != NULL ?
        ((awavma_runtime_t *)context)->config.benefit_calibration_state :
        BENEFIT_CALIBRATION_UNAVAILABLE;
    input.calibration.provenance = context != NULL &&
        ((awavma_runtime_t *)context)->config.benefit_calibration_provenance != NULL ?
        ((awavma_runtime_t *)context)->config.benefit_calibration_provenance :
        "no_cross_numa_production_calibration";
#ifdef AWAVMA_RUNTIME_TESTING
    if (context != NULL &&
        ((awavma_runtime_t *)context)->test_target_case != AWAVMA_RUNTIME_TEST_TARGET_POLICY_LIVE) {
        target_online = true;
        target_permitted = true;
        source_target_valid = true;
        input.target_online = true;
        input.target_permitted = true;
        input.source_target_valid = true;
        input.calibration.state = BENEFIT_CALIBRATION_VALIDATED_TEST_ONLY;
        input.calibration.provenance = "explicit_synthetic_test_fixture";
    }
#else
    (void)context;
#endif
    return benefit_classifier_evaluate(&input, decision);
}

/* Phase 8 is initialized per terminal event because its state paths are app-isolated. */
static bool record_terminal_feedback(void *context, const FeedbackEvent *event, FeedbackResult *result)
{
    awavma_runtime_t *runtime = context;
    FeedbackConfig config = {
        .learning_rate = 0.05, .bias_learning_rate = 0.05, .min_samples = 5,
        .deadband = 0.05, .reward_min = -1.0, .reward_max = 1.0, .decay_lambda = 0.1,
        .min_confidence = 0.5, .history_max_records = 1000, .history_max_days = 30.0,
        .cleanup_interval = 100, .min_weight = 0.0, .max_weight = 2.0,
        .min_bias = -0.25, .max_bias = 0.25, .throughput_weight = 0.4,
        .execution_time_weight = 0.2, .latency_weight = 0.2, .page_fault_weight = 0.2
    };
    char app_dir[4096];
    char state_dir[4096];
    char history_dir[4096];
    char history_path[4096];
    char log_path[4096];
    bool recorded;

#ifdef AWAVMA_RUNTIME_TESTING
    if (runtime != NULL) {
        runtime->test_target_stats.terminal_feedback_calls++;
        runtime->test_target_stats.structural_validation_known = event != NULL &&
            event->structural_validation_known;
        runtime->test_target_stats.structural_validation_succeeded = event != NULL &&
            event->structural_validation_succeeded;
        runtime->test_target_stats.progress_known = event != NULL && event->progress_known;
        runtime->test_target_stats.progress_observed = event != NULL && event->progress_observed;
        runtime->test_target_stats.benefit_known = event != NULL && event->benefit_known;
        runtime->test_target_stats.benefit_classification = event != NULL ?
            event->benefit_classification : INSUFFICIENT_BENEFIT_EVIDENCE;
        if (event != NULL)
            snprintf(runtime->test_target_stats.benefit_reason,
                     sizeof(runtime->test_target_stats.benefit_reason), "%s", event->benefit_reason);
    }
#endif

    if (runtime != NULL && event != NULL)
        release_migration_checkpoint(runtime, event->migration_id);
    if (runtime != NULL && event != NULL)
        release_page_rollback_checkpoint(runtime, event->migration_id);

    if (runtime == NULL || event == NULL || !safe_app_id(event->app_id) ||
        snprintf(app_dir, sizeof(app_dir), "%s/apps/%s", runtime->config.root_dir, event->app_id) >= (int)sizeof(app_dir) ||
        path_join(state_dir, sizeof(state_dir), app_dir, "state") != 0 ||
        path_join(history_dir, sizeof(history_dir), app_dir, "history") != 0 ||
        path_join(history_path, sizeof(history_path), history_dir, "migration_feedback.csv") != 0 ||
        path_join(log_path, sizeof(log_path), app_dir, "feedback.log") != 0 ||
        make_directory(state_dir) != 0 || make_directory(history_dir) != 0)
        return false;
    config.state_dir = state_dir;
    config.history_dir = history_dir;
    config.history_path = history_path;
    config.results_path = NULL;
    config.log_path = log_path;
    if (!Feedback_Init(&config))
        return false;
    recorded = ProcessFeedback(event, result) == FEEDBACK_UPDATE_TERMINAL_RECORDED;
    Feedback_Shutdown();
    return recorded;
}

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

#ifdef AWAVMA_PROFILE
static uint64_t monotonic_ns(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}
#endif

static int path_join(char *buffer, size_t size, const char *left, const char *right)
{
    int written = snprintf(buffer, size, "%s/%s", left, right);

    return written < 0 || (size_t)written >= size ? -1 : 0;
}

static int make_directory(const char *path)
{
    char copy[4096];
    size_t length;

    if (path == NULL || *path == '\0' || strlen(path) >= sizeof(copy))
        return -1;
    snprintf(copy, sizeof(copy), "%s", path);
    length = strlen(copy);
    if (length > 1 && copy[length - 1] == '/')
        copy[length - 1] = '\0';
    for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    return mkdir(copy, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static bool safe_app_id(const char *app_id)
{
    if (app_id == NULL || *app_id == '\0')
        return false;
    for (const unsigned char *cursor = (const unsigned char *)app_id; *cursor != '\0'; cursor++)
        if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_' || *cursor == '-'))
            return false;
    return true;
}

static size_t csv_row_count(const char *path)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    size_t count = 0;

    if (file == NULL)
        return 0;
    if (getline(&line, &capacity, file) >= 0)
        while (getline(&line, &capacity, file) >= 0)
            if (line[0] != '\n' && line[0] != '\0')
                count++;
    free(line);
    fclose(file);
    return count;
}

static int copy_csv_delta(const char *source, const char *destination, size_t start_row)
{
    FILE *input = fopen(source, "r");
    FILE *output = NULL;
    char *line = NULL;
    size_t capacity = 0;
    size_t row = 0;
    int result = -1;

    if (input == NULL)
        return -1;
    output = fopen(destination, "w");
    if (output == NULL)
        goto cleanup;
    if (getline(&line, &capacity, input) < 0)
        goto cleanup;
    fputs(line, output);
    while (getline(&line, &capacity, input) >= 0) {
        if (row++ >= start_row)
            fputs(line, output);
    }
    result = ferror(input) || ferror(output) ? -1 : 0;

cleanup:
    free(line);
    fclose(input);
    if (output != NULL)
        fclose(output);
    return result;
}

/* One-shot Phase 3 sessions restart their local elapsed clock for every sample. */
static int normalize_monitoring_elapsed(const char *source, const char *destination,
                                        uint64_t interval_ms)
{
    FILE *input = fopen(source, "r");
    FILE *output = NULL;
    char *line = NULL;
    size_t capacity = 0;
    char *fields[64];
    int elapsed_column;
    size_t row = 0;
    int result = -1;

    if (input == NULL)
        return -1;
    output = fopen(destination, "w");
    if (output == NULL || getline(&line, &capacity, input) < 0)
        goto cleanup;
    fputs(line, output);
    elapsed_column = column_index(fields, split_csv(line, fields, 64), "elapsed_ms");
    if (elapsed_column < 0)
        goto cleanup;
    while (getline(&line, &capacity, input) >= 0) {
        char elapsed[32];
        size_t field_count = split_csv(line, fields, 64);

        if ((size_t)elapsed_column >= field_count)
            goto cleanup;
        snprintf(elapsed, sizeof(elapsed), "%llu",
                 (unsigned long long)(row++ * interval_ms));
        fields[elapsed_column] = elapsed;
        for (size_t index = 0; index < field_count; index++)
            fprintf(output, "%s%s", index == 0 ? "" : ",", fields[index]);
        fputc('\n', output);
    }
    result = ferror(input) || ferror(output) ? -1 : 0;

cleanup:
    free(line);
    fclose(input);
    if (output != NULL)
        fclose(output);
    return result;
}

static size_t split_csv(char *line, char **fields, size_t capacity)
{
    size_t count = 0;
    char *cursor = line;

    while (cursor != NULL && count < capacity) {
        fields[count++] = cursor;
        cursor = strchr(cursor, ',');
        if (cursor != NULL)
            *cursor++ = '\0';
    }
    if (count > 0) {
        char *last = fields[count - 1];
        last[strcspn(last, "\r\n")] = '\0';
    }
    return count;
}

static int column_index(char **fields, size_t count, const char *name)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(fields[index], name) == 0)
            return (int)index;
    return -1;
}

static const char *field_or_na(char **fields, size_t count, int index)
{
    return index >= 0 && (size_t)index < count && fields[index][0] != '\0' ? fields[index] : "NA";
}

static bool parse_csv_double(const char *text, double *value)
{
    char *end = NULL;

    if (text == NULL || text[0] == '\0' || strcmp(text, "NA") == 0)
        return false;
    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' && isfinite(*value);
}

static bool parse_optional_benefit_value(const char *text, double *value)
{
    return text != NULL && strcmp(text, "-1") != 0 && parse_csv_double(text, value);
}

static bool parse_csv_u64(const char *text, uint64_t *value)
{
    char *end = NULL;

    if (text == NULL || text[0] == '\0' || text[0] == '-' || strcmp(text, "NA") == 0)
        return false;
    errno = 0;
    *value = strtoull(text, &end, 10);
    return errno == 0 && end != text && *end == '\0';
}

static bool parse_csv_action(const char *text, ValidationAction *action)
{
    if (text != NULL && strcmp(text, "MOVE_THREAD") == 0) {
        *action = VALIDATION_ACTION_MOVE_THREAD;
        return true;
    }
    if (text != NULL && strcmp(text, "MOVE_MEMORY") == 0) {
        *action = VALIDATION_ACTION_MOVE_MEMORY;
        return true;
    }
    return false;
}

static GateStatus parse_gate_status(const char *text)
{
    return text != NULL && strcmp(text, "PASS") == 0 ? GATE_PASS :
           text != NULL && strcmp(text, "FAIL") == 0 ? GATE_FAIL :
           text != NULL && strcmp(text, "NOT_APPLICABLE") == 0 ? GATE_NOT_APPLICABLE : GATE_INVALID;
}

/* Phase 6 requires operational evidence that Phase 3/4/5 do not currently emit. */
static int write_validation_input(const char *decision_path, const char *output_path,
                                  const awavma_runtime_record_t *record)
{
    static const char *header =
        "timestamp,migration_id,app_id,pid,entity_id,action,decision_status,classification,"
        "classifier_confidence,stability,sample_count,cpu_utilization,memory_utilization,"
        "concurrency,source_node,destination_node,predicted_gain,estimated_cost,evidence_model,page_locked,"
        "memory_pinned,cooldown_active,thread_locked,migration_in_progress,max_migrations_reached,"
        "phase5_timestamp,phase5_runtime_generation,phase5_evidence_provenance,"
        "phase5_classification_score,phase5_f_access,phase5_f_threshold,phase5_f_gain_memory,"
        "phase5_f_cost_memory,phase5_f_cpu_memory,phase5_f_sharing_memory,"
        "phase5_f_gain_thread,phase5_f_cost_thread,phase5_f_cpu_thread,phase5_f_sharing_thread,"
        "phase5_memory_score_raw,phase5_thread_score_raw,phase5_memory_bias,phase5_thread_bias,"
        "phase5_memory_score_final,phase5_thread_score_final,phase5_decision_margin,phase5_epsilon,phase5_weight_version,"
        "phase5_bias_version\n";
    static const char *phase5_names[] = {
        "classification_score", "f_access", "f_threshold", "f_gain_memory", "f_cost_memory",
        "f_cpu_memory", "f_sharing_memory", "f_gain_thread", "f_cost_thread", "f_cpu_thread",
        "f_sharing_thread", "memory_score_raw", "thread_score_raw", "memory_bias", "thread_bias",
        "memory_score_final", "thread_score_final", "decision_margin", "epsilon", "weight_version", "bias_version"
    };
    FILE *input = fopen(decision_path, "r");
    FILE *output = NULL;
    char *line = NULL;
    size_t line_capacity = 0;
    char *fields[64];
    int timestamp;
    int app_id;
    int pid;
    int entity_id;
    int action;
    int status;
    int classification;
    int predicted_gain;
    int estimated_cost;
    int phase5_columns[sizeof(phase5_names) / sizeof(phase5_names[0])];
    size_t row = 0;
    int result = -1;

    if (input == NULL)
        return -1;
    output = fopen(output_path, "w");
    if (output == NULL || getline(&line, &line_capacity, input) < 0)
        goto cleanup;
    size_t field_count = split_csv(line, fields, 64);
    timestamp = column_index(fields, field_count, "timestamp");
    app_id = column_index(fields, field_count, "app_id");
    pid = column_index(fields, field_count, "pid");
    entity_id = column_index(fields, field_count, "entity_id");
    action = column_index(fields, field_count, "decision");
    status = column_index(fields, field_count, "status");
    classification = column_index(fields, field_count, "classification");
    predicted_gain = column_index(fields, field_count, "predicted_gain");
    estimated_cost = column_index(fields, field_count, "estimated_cost");
    if (timestamp < 0 || app_id < 0 || pid < 0 || entity_id < 0 || action < 0 || status < 0 ||
        classification < 0)
        goto cleanup;
    for (size_t index = 0; index < sizeof(phase5_columns) / sizeof(phase5_columns[0]); index++)
        phase5_columns[index] = column_index(fields, field_count, phase5_names[index]);
    fputs(header, output);
    while (getline(&line, &line_capacity, input) >= 0) {
        char migration_id[128];

        field_count = split_csv(line, fields, 64);
        snprintf(migration_id, sizeof(migration_id), "m_%ld_%llu_%zu", (long)record->pid,
                 (unsigned long long)record->generation, row++);
        fprintf(output, "%s,%s,%s,%s,%s,%s,%s,%s,NA,NA,NA,NA,NA,NA,NA,NA,%s,%s,UTILITY_POLICY_EVIDENCE,NA,NA,NA,NA,NA,NA",
                 field_or_na(fields, field_count, timestamp), migration_id,
                 field_or_na(fields, field_count, app_id), field_or_na(fields, field_count, pid),
                 field_or_na(fields, field_count, entity_id), field_or_na(fields, field_count, action),
                 field_or_na(fields, field_count, status),
                 field_or_na(fields, field_count, classification),
                 field_or_na(fields, field_count, predicted_gain),
                 field_or_na(fields, field_count, estimated_cost));
        fprintf(output, ",%s,%llu,PHASE5_DECISION_ENGINE", field_or_na(fields, field_count, timestamp),
                (unsigned long long)record->generation);
        for (size_t index = 0; index < sizeof(phase5_columns) / sizeof(phase5_columns[0]); index++)
            fprintf(output, ",%s", field_or_na(fields, field_count, phase5_columns[index]));
        fputc('\n', output);
    }
    result = ferror(input) || ferror(output) ? -1 : 0;

cleanup:
    free(line);
    fclose(input);
    if (output != NULL)
        fclose(output);
    return result;
}

static bool csv_has_value(const char *path, const char *column, const char *value)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    char *fields[64];
    int target;
    bool found = false;

    if (file == NULL || getline(&line, &capacity, file) < 0) {
        if (file != NULL)
            fclose(file);
        free(line);
        return false;
    }
    target = column_index(fields, split_csv(line, fields, 64), column);
    while (target >= 0 && getline(&line, &capacity, file) >= 0) {
        size_t count = split_csv(line, fields, 64);

        if ((size_t)target < count && strcmp(fields[target], value) == 0) {
            found = true;
            break;
        }
    }
    free(line);
    fclose(file);
    return found;
}

static bool load_phase5_evidence(const char *path, const char *migration_id,
                                 const awavma_runtime_record_t *record,
                                 ValidationAction expected_action, DecisionData *decision)
{
    static const char *factor_names[] = {
        "phase5_f_access", "phase5_f_threshold", "phase5_f_gain_memory",
        "phase5_f_cost_memory", "phase5_f_cpu_memory", "phase5_f_sharing_memory",
        "phase5_f_gain_thread", "phase5_f_cost_thread", "phase5_f_cpu_thread",
        "phase5_f_sharing_thread"
    };
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    char *fields[64];
    int migration_column;
    int pid_column;
    int action_column;
    int timestamp_column;
    int generation_column;
    int provenance_column;
    int classification_text_column;
    int decision_status_column;
    int app_id_column;
    int classification_column;
    int factor_columns[10];
    int utility_columns[6];
    int epsilon_column;
    int margin_column;
    int weight_column;
    int bias_column;
    int gain_column;
    int cost_column;
    int evidence_model_column;
    bool found = false;

    if (file == NULL || migration_id == NULL || record == NULL || decision == NULL ||
        getline(&line, &capacity, file) < 0)
        goto cleanup;
    size_t count = split_csv(line, fields, 64);
    migration_column = column_index(fields, count, "migration_id");
    pid_column = column_index(fields, count, "pid");
    action_column = column_index(fields, count, "action");
    timestamp_column = column_index(fields, count, "phase5_timestamp");
    generation_column = column_index(fields, count, "phase5_runtime_generation");
    provenance_column = column_index(fields, count, "phase5_evidence_provenance");
    classification_text_column = column_index(fields, count, "classification");
    decision_status_column = column_index(fields, count, "decision_status");
    app_id_column = column_index(fields, count, "app_id");
    classification_column = column_index(fields, count, "phase5_classification_score");
    epsilon_column = column_index(fields, count, "phase5_epsilon");
    margin_column = column_index(fields, count, "phase5_decision_margin");
    weight_column = column_index(fields, count, "phase5_weight_version");
    bias_column = column_index(fields, count, "phase5_bias_version");
    gain_column = column_index(fields, count, "predicted_gain");
    cost_column = column_index(fields, count, "estimated_cost");
    evidence_model_column = column_index(fields, count, "evidence_model");
    utility_columns[0] = column_index(fields, count, "phase5_memory_score_raw");
    utility_columns[1] = column_index(fields, count, "phase5_thread_score_raw");
    utility_columns[2] = column_index(fields, count, "phase5_memory_bias");
    utility_columns[3] = column_index(fields, count, "phase5_thread_bias");
    utility_columns[4] = column_index(fields, count, "phase5_memory_score_final");
    utility_columns[5] = column_index(fields, count, "phase5_thread_score_final");
    for (size_t index = 0; index < 10; index++)
        factor_columns[index] = column_index(fields, count, factor_names[index]);
    if (migration_column < 0 || pid_column < 0 || action_column < 0 || timestamp_column < 0 ||
        generation_column < 0 || provenance_column < 0)
        goto cleanup;
    while (getline(&line, &capacity, file) >= 0) {
        ValidationAction action;
        uint64_t generation;

        count = split_csv(line, fields, 64);
        if ((size_t)migration_column >= count || strcmp(fields[migration_column], migration_id) != 0)
            continue;
        if (found) {
            found = false;
            goto cleanup;
        }
        if ((size_t)pid_column >= count || strtol(fields[pid_column], NULL, 10) != (long)record->pid ||
            (size_t)action_column >= count || !parse_csv_action(fields[action_column], &action) ||
            action != expected_action || (size_t)generation_column >= count ||
            !parse_csv_u64(fields[generation_column], &generation) || generation != record->generation)
            goto cleanup;
        memset(decision, 0, sizeof(*decision));
        decision->pid = (long)record->pid;
        decision->action = action;
        snprintf(decision->migration_id, sizeof(decision->migration_id), "%s", migration_id);
        snprintf(decision->app_id, sizeof(decision->app_id), "%s",
                 field_or_na(fields, count, app_id_column));
        snprintf(decision->action_text, sizeof(decision->action_text), "%s", fields[action_column]);
        snprintf(decision->entity_id, sizeof(decision->entity_id), "%s",
                 field_or_na(fields, count, column_index(fields, count, "entity_id")));
        snprintf(decision->phase5_timestamp, sizeof(decision->phase5_timestamp), "%s",
                 field_or_na(fields, count, timestamp_column));
        snprintf(decision->phase5_evidence_provenance, sizeof(decision->phase5_evidence_provenance),
                 "%s", field_or_na(fields, count, provenance_column));
        snprintf(decision->phase5_classification, sizeof(decision->phase5_classification), "%s",
                 field_or_na(fields, count, classification_text_column));
        snprintf(decision->phase5_decision_status, sizeof(decision->phase5_decision_status), "%s",
                 field_or_na(fields, count, decision_status_column));
        decision->phase5_runtime_generation = generation;
        decision->phase5_runtime_generation_available = true;
        decision->phase5_evidence_available = decision->phase5_timestamp[0] != '\0' &&
            strcmp(decision->phase5_timestamp, "NA") != 0 &&
            strcmp(decision->phase5_evidence_provenance, "PHASE5_DECISION_ENGINE") == 0;
        decision->gain_available = gain_column >= 0 && (size_t)gain_column < count &&
            parse_optional_benefit_value(fields[gain_column], &decision->predicted_gain);
        decision->cost_available = cost_column >= 0 && (size_t)cost_column < count &&
            parse_optional_benefit_value(fields[cost_column], &decision->estimated_cost);
        if (evidence_model_column >= 0 && (size_t)evidence_model_column < count &&
            strcmp(fields[evidence_model_column], "UTILITY_POLICY_EVIDENCE") == 0)
            decision->evidence_model = DECISION_EVIDENCE_UTILITY_POLICY;
        else if (evidence_model_column >= 0 && (size_t)evidence_model_column < count &&
                 strcmp(fields[evidence_model_column], "EMPIRICAL_GAIN_COST_EVIDENCE") == 0)
            decision->evidence_model = DECISION_EVIDENCE_EMPIRICAL_GAIN_COST;
        else
            goto cleanup;
        decision->phase5_classification_score_available = classification_column >= 0 &&
            (size_t)classification_column < count &&
            parse_csv_double(fields[classification_column], &decision->phase5_classification_score);
        for (size_t index = 0; index < 10; index++)
            decision->phase5_factor_available[index] = factor_columns[index] >= 0 &&
                (size_t)factor_columns[index] < count &&
                parse_csv_double(fields[factor_columns[index]], &decision->phase5_factors[index]);
        double *utilities[] = {&decision->phase5_memory_score_raw, &decision->phase5_thread_score_raw,
            &decision->phase5_memory_bias, &decision->phase5_thread_bias,
            &decision->phase5_memory_score_final, &decision->phase5_thread_score_final};
        decision->phase5_utility_available = true;
        for (size_t index = 0; index < 6; index++)
            if (utility_columns[index] < 0 || (size_t)utility_columns[index] >= count ||
                !parse_csv_double(fields[utility_columns[index]], utilities[index]))
                decision->phase5_utility_available = false;
        decision->phase5_epsilon_available = epsilon_column >= 0 && (size_t)epsilon_column < count &&
            parse_csv_double(fields[epsilon_column], &decision->phase5_epsilon);
        decision->phase5_decision_margin_available = margin_column >= 0 &&
            (size_t)margin_column < count &&
            parse_csv_double(fields[margin_column], &decision->phase5_decision_margin);
        uint64_t weight;
        uint64_t bias;
        decision->phase5_versions_available = weight_column >= 0 && bias_column >= 0 &&
            (size_t)weight_column < count && (size_t)bias_column < count &&
            parse_csv_u64(fields[weight_column], &weight) && parse_csv_u64(fields[bias_column], &bias) &&
            weight <= UINT_MAX && bias <= UINT_MAX;
        if (decision->phase5_versions_available) {
            decision->phase5_weight_version = (unsigned)weight;
            decision->phase5_bias_version = (unsigned)bias;
        }
        found = true;
    }

cleanup:
    free(line);
    if (file != NULL)
        fclose(file);
    return found;
}

/* The adapter migration ID is the Phase 5/6 join key; reject duplicate or stale joins. */
static int approved_migration_evidence(const char *validation_input_path, const char *validation_path,
                                       const awavma_runtime_record_t *record,
                                       ValidationAction *action, DecisionData *decision,
                                       ValidationResult *validation)
{
    FILE *file = fopen(validation_path, "r");
    char *line = NULL;
    size_t capacity = 0;
    char *fields[64];
    int migration_column;
    int action_column;
    int decision_column;
    int pid_column;
    int confidence_column;
    int roi_column;
    int safety_column;
    int validation_score_column;
    int confidence_status_column;
    int roi_status_column;
    int safety_status_column;
    int validation_status_column;
    int timestamp_column;
    int app_id_column;
    int entity_id_column;
    int matches = 0;

    if (file == NULL || record == NULL || action == NULL || decision == NULL || validation == NULL ||
        getline(&line, &capacity, file) < 0)
        goto error;
    size_t count = split_csv(line, fields, 64);
    migration_column = column_index(fields, count, "migration_id");
    action_column = column_index(fields, count, "action");
    decision_column = column_index(fields, count, "final_decision");
    pid_column = column_index(fields, count, "pid");
    confidence_column = column_index(fields, count, "confidence_score");
    roi_column = column_index(fields, count, "roi_score");
    safety_column = column_index(fields, count, "safety_score");
    validation_score_column = column_index(fields, count, "validation_score");
    confidence_status_column = column_index(fields, count, "confidence_status");
    roi_status_column = column_index(fields, count, "roi_status");
    safety_status_column = column_index(fields, count, "safety_status");
    validation_status_column = column_index(fields, count, "validation_status");
    timestamp_column = column_index(fields, count, "timestamp");
    app_id_column = column_index(fields, count, "app_id");
    entity_id_column = column_index(fields, count, "entity_id");
    if (migration_column < 0 || action_column < 0 || decision_column < 0 || pid_column < 0 ||
        confidence_column < 0 || roi_column < 0 || safety_column < 0 || validation_score_column < 0 ||
        confidence_status_column < 0 || roi_status_column < 0 || safety_status_column < 0 ||
        validation_status_column < 0 || timestamp_column < 0 || app_id_column < 0 || entity_id_column < 0)
        goto error;
    while (getline(&line, &capacity, file) >= 0) {
        ValidationAction parsed;

        count = split_csv(line, fields, 64);
        if ((size_t)decision_column >= count || (size_t)pid_column >= count ||
            strcmp(fields[decision_column], "APPROVED") != 0 ||
            strtol(fields[pid_column], NULL, 10) != (long)record->pid)
            continue;
        if ((size_t)action_column >= count || (size_t)migration_column >= count ||
            (size_t)confidence_column >= count || (size_t)roi_column >= count ||
            (size_t)safety_column >= count || (size_t)validation_score_column >= count ||
            (size_t)confidence_status_column >= count || (size_t)roi_status_column >= count ||
            (size_t)safety_status_column >= count || (size_t)validation_status_column >= count ||
            (size_t)timestamp_column >= count || (size_t)app_id_column >= count ||
            (size_t)entity_id_column >= count ||
            !parse_csv_action(fields[action_column], &parsed) || fields[migration_column][0] == '\0')
            goto error;
        if (++matches != 1)
            goto error;
        memset(validation, 0, sizeof(*validation));
        validation->pid = (long)record->pid;
        validation->action = parsed;
        snprintf(validation->timestamp, sizeof(validation->timestamp), "%s", fields[timestamp_column]);
        snprintf(validation->migration_id, sizeof(validation->migration_id), "%s", fields[migration_column]);
        snprintf(validation->app_id, sizeof(validation->app_id), "%s", fields[app_id_column]);
        snprintf(validation->entity_id, sizeof(validation->entity_id), "%s", fields[entity_id_column]);
        snprintf(validation->final_decision, sizeof(validation->final_decision), "APPROVED");
        snprintf(validation->validation_status, sizeof(validation->validation_status), "%s",
                 fields[validation_status_column]);
        validation->confidence_status = parse_gate_status(fields[confidence_status_column]);
        validation->roi_status = parse_gate_status(fields[roi_status_column]);
        validation->safety_status = parse_gate_status(fields[safety_status_column]);
        (void)parse_csv_double(fields[confidence_column], &validation->confidence_score);
        (void)parse_csv_double(fields[roi_column], &validation->roi_score);
        (void)parse_csv_double(fields[safety_column], &validation->safety_score);
        (void)parse_csv_double(fields[validation_score_column], &validation->validation_score);
        *action = parsed;
    }
    free(line);
    fclose(file);
    if (matches == 0)
        return 0;
    return load_phase5_evidence(validation_input_path, validation->migration_id, record, *action, decision) ?
        1 : -1;

error:
    free(line);
    if (file != NULL)
        fclose(file);
    return -1;
}

#ifdef AWAVMA_RUNTIME_TESTING
int awavma_runtime_test_write_validation_input(const char *decision_path, const char *output_path,
                                               const awavma_runtime_record_t *record)
{
    return write_validation_input(decision_path, output_path, record);
}

int awavma_runtime_test_load_benefit_evidence(const char *validation_input_path,
                                              const char *validation_path,
                                              const awavma_runtime_record_t *record,
                                              DecisionData *decision, ValidationResult *validation)
{
    ValidationAction action;

    return approved_migration_evidence(validation_input_path, validation_path, record, &action,
                                       decision, validation);
}
#endif

static int run_program(const awavma_runtime_t *runtime, const char *cycle_dir,
                       const char *phase, const char *application, pid_t pid,
                       uint64_t generation, char *const arguments[])
{
    char stdout_path[4096];
    char stderr_path[4096];
    char child_profile_path[4096];
    char prepare_component[64];
    char fork_component[64];
    pid_t child;
    int status;
    int stdout_fd;
    int stderr_fd;
    monitor_profile_scope_t prepare_profile;
    monitor_profile_scope_t fork_profile;

    snprintf(prepare_component, sizeof(prepare_component), "%s_parent_prepare", phase);
    snprintf(fork_component, sizeof(fork_component), "%s_fork_wait_wall", phase);
    monitor_profile_scope_begin(&prepare_profile, "pipeline", prepare_component, application, pid, generation);
    if (snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout.log", cycle_dir, phase) >=
            (int)sizeof(stdout_path) ||
        snprintf(stderr_path, sizeof(stderr_path), "%s/%s.stderr.log", cycle_dir, phase) >=
            (int)sizeof(stderr_path) ||
        snprintf(child_profile_path, sizeof(child_profile_path), "%s/%s.profile.csv", cycle_dir, phase) >=
            (int)sizeof(stderr_path))
    {
        monitor_profile_scope_end(&prepare_profile, "ERROR");
        return -1;
    }
    stdout_fd = open(stdout_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    stderr_fd = open(stderr_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (stdout_fd < 0 || stderr_fd < 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        monitor_profile_scope_end(&prepare_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&prepare_profile, "OK");
    monitor_profile_scope_begin(&fork_profile, "pipeline", fork_component, application, pid, generation);
    child = fork();
    if (child == 0) {
        dup2(stdout_fd, STDOUT_FILENO);
        dup2(stderr_fd, STDERR_FILENO);
        close(stdout_fd);
        close(stderr_fd);
        if (setenv("AWAVMA_PROFILE_PATH", child_profile_path, 1) != 0)
            _exit(127);
        execv(arguments[0], arguments);
        _exit(127);
    }
    close(stdout_fd);
    close(stderr_fd);
    if (child < 0) {
        monitor_profile_scope_end(&fork_profile, "ERROR");
        return -1;
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) {
            monitor_profile_scope_end(&fork_profile, "ERROR");
            return -1;
        }
    (void)runtime;
    int result = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    monitor_profile_scope_end(&fork_profile, result == 0 ? "OK" : "ERROR");
    return result;
}

static int run_classifier_in_process(const char *cycle_dir, const classifier_config_t *config,
                                     classifier_summary_t *summary)
{
    char stdout_path[4096];
    char stderr_path[4096];
    int stdout_fd;
    int stderr_fd;
    int saved_stdout;
    int saved_stderr;
    int result;

    if (snprintf(stdout_path, sizeof(stdout_path), "%s/phase4.stdout.log", cycle_dir) >= (int)sizeof(stdout_path) ||
        snprintf(stderr_path, sizeof(stderr_path), "%s/phase4.stderr.log", cycle_dir) >= (int)sizeof(stderr_path))
        return -1;
    stdout_fd = open(stdout_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    stderr_fd = open(stderr_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (stdout_fd < 0 || stderr_fd < 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout < 0 || saved_stderr < 0 || dup2(stdout_fd, STDOUT_FILENO) < 0 ||
        dup2(stderr_fd, STDERR_FILENO) < 0) {
        if (saved_stdout >= 0)
            dup2(saved_stdout, STDOUT_FILENO);
        if (saved_stderr >= 0)
            dup2(saved_stderr, STDERR_FILENO);
        if (saved_stdout >= 0) close(saved_stdout);
        if (saved_stderr >= 0) close(saved_stderr);
        close(stdout_fd);
        close(stderr_fd);
        return -1;
    }
    close(stdout_fd);
    close(stderr_fd);
    fprintf(stdout, "AWAVMA in-process Phase 4 classifier\n");
    result = classifier_run(config, summary);
    fflush(stdout);
    fflush(stderr);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);
    return result;
}

static awavma_runtime_record_t *runtime_record(awavma_runtime_t *runtime,
                                                const runtime_monitor_record_t *monitor_record)
{
    for (size_t index = 0; index < runtime->record_count; index++)
        if (runtime->records[index].pid == monitor_record->pid &&
            runtime->records[index].start_time_ticks == monitor_record->start_time_ticks &&
            strcmp(runtime->records[index].app_id, monitor_record->app_id) == 0)
            return &runtime->records[index];
    if (runtime->record_count >= runtime->config.max_applications)
        return NULL;
    awavma_runtime_record_t *record = &runtime->records[runtime->record_count++];
    memset(record, 0, sizeof(*record));
    snprintf(record->app_id, sizeof(record->app_id), "%s", monitor_record->app_id);
    record->pid = monitor_record->pid;
    record->start_time_ticks = monitor_record->start_time_ticks;
    record->status = AWAVMA_RUNTIME_MONITORING;
    snprintf(record->detail, sizeof(record->detail), "monitoring");
    return record;
}

static void set_status(awavma_runtime_record_t *record, awavma_runtime_status_t status,
                       const char *detail)
{
    record->status = status;
    snprintf(record->detail, sizeof(record->detail), "%.*s", (int)(sizeof(record->detail) - 1), detail);
}

static int write_runtime_results(const awavma_runtime_t *runtime)
{
    char temporary[4096];
    FILE *file;

    if (snprintf(temporary, sizeof(temporary), "%s.tmp", runtime->runtime_results_path) >=
        (int)sizeof(temporary))
        return -1;
    file = fopen(temporary, "w");
    if (file == NULL)
        return -1;
    fprintf(file, "app_id,pid,start_time_ticks,generation,phase3_samples,phase5_committed_rows,status,detail\n");
    for (size_t index = 0; index < runtime->record_count; index++) {
        const awavma_runtime_record_t *record = &runtime->records[index];

        fprintf(file, "%s,%ld,%llu,%llu,%zu,%zu,%s,%s\n", record->app_id, (long)record->pid,
                (unsigned long long)record->start_time_ticks,
                (unsigned long long)record->generation, record->phase3_samples,
                record->phase5_committed_rows, awavma_runtime_status_name(record->status),
                record->detail);
    }
    if (fclose(file) != 0 || rename(temporary, runtime->runtime_results_path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int process_application(awavma_runtime_t *runtime, awavma_runtime_record_t *record,
                               uint64_t pipeline_ready_ns)
{
    char phase3_path[4096];
    char app_dir[4096];
    char cycles_dir[4096];
    char cycle_dir[4096];
    char classification_path[4096];
    char normalized_monitoring_path[4096];
    char classification_delta_path[4096];
    char decision_path[4096];
    char validation_input_path[4096];
    char validation_path[4096];
    char state_dir[4096];
    char history_dir[4096];
    char log_path[4096];
    char classifier[4096];
    char decision[4096];
    char validation[4096];
    char generation[64];
    size_t samples;
    monitor_profile_scope_t coordinator_profile;
    monitor_profile_scope_t scan_profile;
    monitor_profile_scope_t artifact_profile;
    monitor_profile_scope_t normalize_profile;
    monitor_profile_scope_t delta_profile;
    monitor_profile_scope_t adapter_profile;
    monitor_profile_scope_t result_profile;

    if (!safe_app_id(record->app_id) ||
        snprintf(phase3_path, sizeof(phase3_path), "%s/phase3/%s_monitoring.csv",
                 runtime->config.root_dir, record->app_id) >= (int)sizeof(phase3_path)) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "invalid Phase 3 artifact path");
        return -1;
    }
    monitor_profile_scope_begin(&scan_profile, "pipeline", "phase3_artifact_scan", record->app_id,
                                record->pid, record->generation + 1);
    samples = csv_row_count(phase3_path);
    monitor_profile_scope_end(&scan_profile, "OK");
    record->phase3_samples = samples;
    if (record->status == AWAVMA_RUNTIME_AMBIGUOUS)
        return 0;
    if (samples <= record->phase5_committed_rows)
        return 0;
#ifdef AWAVMA_PROFILE
    monitor_profile_event("pipeline", "pipeline_serial_wait", record->app_id, record->pid,
                          record->generation + 1, pipeline_ready_ns, monotonic_ns(), "OK");
#else
    (void)pipeline_ready_ns;
#endif
    monitor_profile_scope_begin(&coordinator_profile, "coordinator", "coordinator_application_total",
                                record->app_id, record->pid, record->generation + 1);
    monitor_profile_scope_begin(&artifact_profile, "coordinator", "coordinator_artifact_prepare",
                                record->app_id, record->pid, record->generation + 1);
    if (snprintf(app_dir, sizeof(app_dir), "%s/apps/%s", runtime->config.root_dir, record->app_id) >=
            (int)sizeof(app_dir) ||
        snprintf(generation, sizeof(generation), "%llu", (unsigned long long)(record->generation + 1)) >=
            (int)sizeof(generation) ||
        path_join(cycles_dir, sizeof(cycles_dir), app_dir, "cycles") != 0 ||
        path_join(cycle_dir, sizeof(cycle_dir), cycles_dir, generation) != 0 ||
        path_join(state_dir, sizeof(state_dir), app_dir, "state") != 0 ||
        path_join(history_dir, sizeof(history_dir), app_dir, "history") != 0 ||
        path_join(log_path, sizeof(log_path), app_dir, "decision.log") != 0 ||
        make_directory(cycle_dir) != 0 || make_directory(state_dir) != 0 ||
        make_directory(history_dir) != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "cannot create application artifacts");
        monitor_profile_scope_end(&artifact_profile, "ERROR");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    if (path_join(normalized_monitoring_path, sizeof(normalized_monitoring_path), cycle_dir,
                  "monitoring_normalized.csv") != 0 ||
        path_join(classification_path, sizeof(classification_path), cycle_dir, "classification_full.csv") != 0 ||
        path_join(classification_delta_path, sizeof(classification_delta_path), cycle_dir, "classification_delta.csv") != 0 ||
        path_join(decision_path, sizeof(decision_path), cycle_dir, "decision.csv") != 0 ||
        path_join(validation_input_path, sizeof(validation_input_path), cycle_dir, "validation_input.csv") != 0 ||
        path_join(validation_path, sizeof(validation_path), cycle_dir, "validation.csv") != 0 ||
        path_join(classifier, sizeof(classifier), runtime->config.bin_dir, "classifier") != 0 ||
        path_join(decision, sizeof(decision), runtime->config.bin_dir, "decision") != 0 ||
        path_join(validation, sizeof(validation), runtime->config.bin_dir, "validation") != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "artifact path exceeds limit");
        monitor_profile_scope_end(&artifact_profile, "ERROR");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&artifact_profile, "OK");
    char *classifier_args[] = {classifier, "--input", normalized_monitoring_path, "--output", classification_path, NULL};
    monitor_profile_scope_begin(&normalize_profile, "pipeline", "monitoring_normalize_io", record->app_id,
                                record->pid, record->generation + 1);
    if (normalize_monitoring_elapsed(phase3_path, normalized_monitoring_path,
                                    runtime->config.monitor_interval_ms) != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "Phase 4 failed");
        monitor_profile_scope_end(&normalize_profile, "ERROR");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&normalize_profile, "OK");
    monitor_profile_scope_t phase4_profile;
    monitor_profile_scope_begin(&phase4_profile, "pipeline", "phase4_execution", record->app_id,
                                record->pid, record->generation + 1);
    int phase4_result;
    if (runtime->config.phase4_mode == AWAVMA_PHASE4_IN_PROCESS) {
        classifier_config_t classifier_config;
        classifier_summary_t classifier_summary;
        monitor_profile_scope_t direct_profile;

        classifier_config_default(&classifier_config);
        classifier_config.input_path = normalized_monitoring_path;
        classifier_config.output_path = classification_path;
        monitor_profile_scope_begin(&direct_profile, "pipeline", "phase4_direct_api_total",
                                    record->app_id, record->pid, record->generation + 1);
        phase4_result = run_classifier_in_process(cycle_dir, &classifier_config, &classifier_summary);
        monitor_profile_scope_end(&direct_profile, phase4_result == 0 ? "OK" : "ERROR");
    } else {
        phase4_result = run_program(runtime, cycle_dir, "phase4", record->app_id, record->pid,
                                    record->generation + 1, classifier_args);
    }
    monitor_profile_scope_end(&phase4_profile, phase4_result == 0 ? "OK" : "ERROR");
    if (phase4_result != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "Phase 4 failed");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_begin(&delta_profile, "pipeline", "classification_delta_io", record->app_id,
                                record->pid, record->generation + 1);
    int delta_result = copy_csv_delta(classification_path, classification_delta_path,
                                      record->phase5_committed_rows);
    monitor_profile_scope_end(&delta_profile, delta_result == 0 ? "OK" : "ERROR");
    if (delta_result != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "Phase 4 failed");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    char *decision_args[] = {decision, "--input", classification_delta_path, "--output", decision_path,
                             "--app-id", record->app_id, "--state-dir", state_dir,
                             "--history-dir", history_dir, "--log", log_path, NULL};
    record->generation++;
    monitor_profile_scope_t phase5_profile;
    monitor_profile_scope_begin(&phase5_profile, "pipeline", "phase5_execution", record->app_id,
                                record->pid, record->generation);
    int phase5_result = run_program(runtime, cycle_dir, "phase5", record->app_id, record->pid,
                                    record->generation, decision_args);
    monitor_profile_scope_end(&phase5_profile, phase5_result == 0 ? "OK" : "ERROR");
    if (phase5_result != 0) {
        set_status(record, AWAVMA_RUNTIME_AMBIGUOUS, "Phase 5 outcome is unknown");
        monitor_profile_scope_end(&coordinator_profile, "AMBIGUOUS");
        return -1;
    }
    record->phase5_committed_rows = samples;
    monitor_profile_scope_begin(&adapter_profile, "pipeline", "validation_input_adapt_io", record->app_id,
                                record->pid, record->generation);
    if (write_validation_input(decision_path, validation_input_path, record) != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "cannot adapt Phase 5 output for Phase 6");
        monitor_profile_scope_end(&adapter_profile, "ERROR");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&adapter_profile, "OK");
    char validation_history[4096];
    char validation_log[4096];
    if (path_join(validation_history, sizeof(validation_history), history_dir, "validation.csv") != 0 ||
        path_join(validation_log, sizeof(validation_log), app_dir, "validation.log") != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "validation path exceeds limit");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    char *validation_args[] = {validation, "--input", validation_input_path, "--output", validation_path,
                               "--history", validation_history, "--log", validation_log,
                               "--config", (char *)runtime->config.phase_config_path, NULL};
    monitor_profile_scope_t phase6_profile;
    monitor_profile_scope_begin(&phase6_profile, "pipeline", "phase6_execution", record->app_id,
                                record->pid, record->generation);
    int phase6_result = run_program(runtime, cycle_dir, "phase6", record->app_id, record->pid,
                                    record->generation, validation_args);
    monitor_profile_scope_end(&phase6_profile, phase6_result == 0 ? "OK" : "ERROR");
    if (phase6_result != 0) {
        set_status(record, AWAVMA_RUNTIME_AMBIGUOUS, "Phase 6 outcome is unknown");
        monitor_profile_scope_end(&coordinator_profile, "AMBIGUOUS");
        return -1;
    }
    monitor_profile_scope_begin(&result_profile, "pipeline", "pipeline_result_parse_io", record->app_id,
                                record->pid, record->generation);
    ValidationAction approved_action = VALIDATION_ACTION_INSUFFICIENT;
    DecisionData approved_phase5 = {0};
    ValidationResult approved_phase6 = {0};
    bool insufficient = csv_has_value(decision_path, "decision", "INSUFFICIENT_DECISION_SIGNAL");
    int approved_action_result = insufficient ? 0 :
        approved_migration_evidence(validation_input_path, validation_path, record, &approved_action,
                                    &approved_phase5, &approved_phase6);
    bool approved = approved_action_result == 1;
    monitor_profile_scope_end(&result_profile, "OK");
    if (insufficient)
        set_status(record, AWAVMA_RUNTIME_INSUFFICIENT, "unavailable decision evidence");
    else if (approved_action_result < 0)
        set_status(record, AWAVMA_RUNTIME_AMBIGUOUS,
                   "Phase 5/6 did not provide one unambiguous approved migration intent");
    else if (approved)
        if (runtime->migration_safety != NULL) {
            MigrationSafetyRequest request = {0};
            MigrationSafetyResult safety_result;
            snprintf(request.app_id, sizeof(request.app_id), "%s", record->app_id);
            request.pid = record->pid;
            request.start_time_ticks = record->start_time_ticks;
            request.action = approved_action;
            request.source_numa_node = -1;
            request.destination_numa_node = -1;
            request.target_requires_cross_node = true;
            request.system_safe = true;
            request.migration_request.pid = record->pid;
            request.migration_request.start_time_ticks = record->start_time_ticks;
            request.migration_request.start_time_ticks_available = true;
            request.migration_request.phase5_decision = approved_phase5;
            request.migration_request.phase6_validation = approved_phase6;
            request.migration_request.benefit_evidence_runtime_generation = record->generation;
            if (migration_safety_manager_attempt(runtime->migration_safety, &request, &safety_result))
                set_status(record, AWAVMA_RUNTIME_REJECTED, safety_result.detail);
            else
                set_status(record, AWAVMA_RUNTIME_MIGRATION_METADATA_UNAVAILABLE,
                            "Phase 7 safety manager is unavailable");
        } else
            set_status(record, AWAVMA_RUNTIME_MIGRATION_METADATA_UNAVAILABLE,
                       "Phase 7 disabled: execution metadata unavailable");
    else
        set_status(record, AWAVMA_RUNTIME_REJECTED, "Phase 6 did not approve migration");
#ifdef AWAVMA_PROFILE
    monitor_profile_counter("pipeline", "pipeline_completion", record->app_id, record->pid,
                            record->generation, 1, awavma_runtime_status_name(record->status));
#endif
    monitor_profile_scope_end(&coordinator_profile, "OK");
    return 0;
}

static int process_pipeline(awavma_runtime_t *runtime)
{
    runtime_monitor_record_t *monitor_records;
    size_t monitor_count;
    uint64_t pipeline_ready_ns = 0;
    monitor_profile_scope_t pipeline_profile;
    monitor_profile_scope_t snapshot_profile;
    monitor_profile_scope_t publish_profile;

#ifdef AWAVMA_PROFILE
    pipeline_ready_ns = monotonic_ns();
#endif
    monitor_profile_scope_begin(&pipeline_profile, "pipeline", "pipeline_serial_total", NULL, -1, 0);

    monitor_profile_scope_begin(&snapshot_profile, "pipeline", "pipeline_snapshot_prepare", NULL, -1, 0);
    monitor_records = calloc(runtime->config.max_applications, sizeof(*monitor_records));
    if (monitor_records == NULL) {
        monitor_profile_scope_end(&snapshot_profile, "ERROR");
        monitor_profile_scope_end(&pipeline_profile, "ERROR");
        return ENOMEM;
    }
    monitor_count = runtime_monitor_snapshot(runtime->monitor, monitor_records,
                                              runtime->config.max_applications);
    monitor_profile_scope_end(&snapshot_profile, "OK");
    for (size_t index = 0; index < monitor_count; index++) {
        awavma_runtime_record_t *record = runtime_record(runtime, &monitor_records[index]);
        application_manager_record_t application;

        if (record == NULL)
            continue;
        if (!application_manager_lookup_identity(runtime->manager, record->pid,
                                                 record->start_time_ticks, &application) ||
            application.status != APPLICATION_MANAGER_ACTIVE) {
            set_status(record, AWAVMA_RUNTIME_TARGET_GONE, "identity no longer active");
            continue;
        }
        if (monitor_records[index].status == RUNTIME_MONITOR_TARGET_GONE ||
            monitor_records[index].status == RUNTIME_MONITOR_TERMINATED) {
            set_status(record, AWAVMA_RUNTIME_TARGET_GONE, "monitor target terminated");
            continue;
        }
        if (monitor_records[index].status == RUNTIME_MONITOR_ERROR) {
            set_status(record, AWAVMA_RUNTIME_ERROR, "Phase 3 sampling failed");
            continue;
        }
        (void)process_application(runtime, record, pipeline_ready_ns);
    }
    free(monitor_records);
    monitor_profile_scope_begin(&publish_profile, "pipeline", "runtime_results_publish_io", NULL, -1, 0);
    int result = write_runtime_results(runtime) == 0 ? 0 : EIO;
    monitor_profile_scope_end(&publish_profile, result == 0 ? "OK" : "ERROR");
    monitor_profile_scope_end(&pipeline_profile, result == 0 ? "OK" : "ERROR");
    return result;
}

void awavma_runtime_config_default(awavma_runtime_config_t *config)
{
    if (config == NULL)
        return;
    config->monitor_interval_ms = DEFAULT_MONITOR_INTERVAL_MS;
    config->discovery_interval_ms = 250;
    config->evaluation_interval_ms = DEFAULT_EVALUATION_INTERVAL_MS;
    config->max_applications = DEFAULT_MAX_APPLICATIONS;
    config->worker_count = DEFAULT_WORKERS;
    config->queue_capacity = DEFAULT_QUEUE_CAPACITY;
    config->phase4_mode = AWAVMA_PHASE4_SUBPROCESS;
    config->root_dir = DEFAULT_ROOT_DIR;
    config->bin_dir = DEFAULT_BIN_DIR;
    config->phase_config_path = DEFAULT_PHASE_CONFIG;
    config->migration_safety_enabled = false;
    config->migration_execution_enabled = false;
    config->benefit_calibration_state = BENEFIT_CALIBRATION_UNAVAILABLE;
    config->benefit_calibration_provenance = "no_cross_numa_production_calibration";
    application_discovery_config_default(&config->discovery_config);
    config->application_filter = NULL;
    config->application_filter_context = NULL;
}

const char *awavma_runtime_status_name(awavma_runtime_status_t status)
{
    switch (status) {
    case AWAVMA_RUNTIME_MONITORING: return "MONITORING";
    case AWAVMA_RUNTIME_INSUFFICIENT: return "INSUFFICIENT";
    case AWAVMA_RUNTIME_REJECTED: return "REJECTED";
    case AWAVMA_RUNTIME_MIGRATION_METADATA_UNAVAILABLE: return "MIGRATION_METADATA_UNAVAILABLE";
    case AWAVMA_RUNTIME_FEEDBACK_PENDING: return "FEEDBACK_PENDING";
    case AWAVMA_RUNTIME_TARGET_GONE: return "TARGET_GONE";
    case AWAVMA_RUNTIME_ERROR: return "ERROR";
    case AWAVMA_RUNTIME_AMBIGUOUS: return "AMBIGUOUS";
    default: return "UNKNOWN";
    }
}

awavma_runtime_t *awavma_runtime_create(void)
{
    return calloc(1, sizeof(awavma_runtime_t));
}

int awavma_runtime_init(awavma_runtime_t *runtime, const awavma_runtime_config_t *config)
{
    awavma_runtime_config_t defaults;
    application_manager_config_t manager_config;
    worker_pool_config_t pool_config;
    runtime_monitor_config_t monitor_config;
    char path[4096];

    if (runtime == NULL)
        return EINVAL;
    if (config == NULL) {
        awavma_runtime_config_default(&defaults);
        config = &defaults;
    }
    if (config->monitor_interval_ms == 0 || config->evaluation_interval_ms == 0 ||
        config->max_applications == 0 || config->worker_count == 0 || config->queue_capacity == 0 ||
        (config->phase4_mode != AWAVMA_PHASE4_SUBPROCESS &&
         config->phase4_mode != AWAVMA_PHASE4_IN_PROCESS) ||
        config->root_dir == NULL || config->bin_dir == NULL || config->phase_config_path == NULL)
        return EINVAL;
    memset(runtime, 0, sizeof(*runtime));
    runtime->config = *config;
    if (make_directory(config->root_dir) != 0 ||
        snprintf(path, sizeof(path), "%s/logs", config->root_dir) >= (int)sizeof(path) ||
        make_directory(path) != 0 ||
        snprintf(path, sizeof(path), "%s/phase3", config->root_dir) >= (int)sizeof(path) ||
        make_directory(path) != 0 ||
        snprintf(path, sizeof(path), "%s/apps", config->root_dir) >= (int)sizeof(path) ||
        make_directory(path) != 0)
        return EIO;
    runtime->records = calloc(config->max_applications, sizeof(*runtime->records));
    runtime->manager = application_manager_create();
    runtime->pool = worker_pool_create();
    runtime->monitor = runtime_monitor_create();
    if (runtime->records == NULL || runtime->manager == NULL || runtime->pool == NULL || runtime->monitor == NULL)
        goto fail;
    application_manager_config_default(&manager_config);
    manager_config.max_applications = config->max_applications;
    snprintf(runtime->manager_log_path, sizeof(runtime->manager_log_path),
             "%s/logs/application_manager.log", config->root_dir);
    snprintf(runtime->manager_results_path, sizeof(runtime->manager_results_path),
             "%s/application_manager_results.csv", config->root_dir);
    snprintf(runtime->manager_table_path, sizeof(runtime->manager_table_path),
             "%s/application_manager_table.csv", config->root_dir);
    manager_config.log_path = runtime->manager_log_path;
    manager_config.results_path = runtime->manager_results_path;
    manager_config.table_path = runtime->manager_table_path;
    if (application_manager_init(runtime->manager, &manager_config) != 0) {
        goto fail;
    }
    worker_pool_config_default(&pool_config);
    pool_config.worker_count = config->worker_count;
    pool_config.queue_capacity = config->queue_capacity;
    pool_config.log_path = NULL;
    if (worker_pool_init(runtime->pool, &pool_config) != WORKER_POOL_SUCCESS)
        goto fail;
    runtime_monitor_config_default(&monitor_config);
    monitor_config.monitor_interval_ms = config->monitor_interval_ms;
    monitor_config.discovery_interval_ms = config->discovery_interval_ms;
    monitor_config.max_applications = config->max_applications;
    snprintf(runtime->monitor_dir, sizeof(runtime->monitor_dir), "%s/phase3", config->root_dir);
    snprintf(runtime->monitor_results_path, sizeof(runtime->monitor_results_path),
             "%s/runtime_monitor_results.csv", config->root_dir);
    snprintf(runtime->monitor_log_path, sizeof(runtime->monitor_log_path),
             "%s/logs/runtime_monitor.log", config->root_dir);
    snprintf(runtime->runtime_results_path, sizeof(runtime->runtime_results_path),
             "%s/runtime_results.csv", config->root_dir);
    monitor_config.results_dir = runtime->monitor_dir;
    monitor_config.results_path = runtime->monitor_results_path;
    monitor_config.log_path = runtime->monitor_log_path;
    monitor_config.discovery_config = config->discovery_config;
    monitor_config.application_filter = config->application_filter;
    monitor_config.application_filter_context = config->application_filter_context;
    if (runtime_monitor_init(runtime->monitor, runtime->manager, runtime->pool, &monitor_config) != 0) {
        goto fail;
    }
    if (config->migration_safety_enabled) {
        MigrationSafetyConfig safety_config;
        MigrationConfig migration_config = {
            .history_max_records = 1000, .history_max_days = 30.0,
            .history_decay_lambda = 0.1, .cleanup_interval = 100,
            .cooldown_ms = 1000, .lock_timeout_ms = 0, .verification_enabled = true
        };
        char migration_results[4096];
        char migration_history[4096];
        char migration_log[4096];
        char migration_state[4096];

        runtime->migration_safety = migration_safety_manager_create();
        migration_safety_config_default(&safety_config);
        safety_config.enabled = true;
        safety_config.execution_enabled = config->migration_execution_enabled;
        safety_config.max_applications = config->max_applications;
        safety_config.history_path = runtime->migration_safety_history_path;
        safety_config.identity_fn = runtime_identity_matches;
        safety_config.execute_fn = runtime_execute_migration;
        safety_config.capture_before_fn = runtime_capture_before_migration;
        safety_config.validate_fn = runtime_validate_migration;
        safety_config.rollback_fn = runtime_rollback_migration;
        safety_config.target_fn = runtime_get_migration_target;
        safety_config.page_checkpoint_fn = runtime_capture_page_checkpoint;
        safety_config.benefit_fn = runtime_classify_benefit;
        safety_config.feedback_fn = record_terminal_feedback;
        safety_config.callback_context = runtime;
        if (snprintf(runtime->migration_safety_history_path,
                     sizeof(runtime->migration_safety_history_path), "%s/migration_safety_history.csv",
                     config->root_dir) >= (int)sizeof(runtime->migration_safety_history_path) ||
            snprintf(migration_results, sizeof(migration_results), "%s/migration_results.csv", config->root_dir) >= (int)sizeof(migration_results) ||
            snprintf(migration_history, sizeof(migration_history), "%s/migration_history.csv", config->root_dir) >= (int)sizeof(migration_history) ||
            snprintf(migration_log, sizeof(migration_log), "%s/logs/migration.log", config->root_dir) >= (int)sizeof(migration_log) ||
            snprintf(migration_state, sizeof(migration_state), "%s/migration_state.csv", config->root_dir) >= (int)sizeof(migration_state) ||
            runtime->migration_safety == NULL ||
            !migration_safety_manager_init(runtime->migration_safety, &safety_config)) {
            migration_safety_manager_destroy(runtime->migration_safety);
            runtime->migration_safety = NULL;
        } else {
            migration_config.results_path = migration_results;
            migration_config.history_path = migration_history;
            migration_config.log_path = migration_log;
            migration_config.state_path = migration_state;
            runtime->migration_initialized = Migration_Init(&migration_config);
        }
    }
    runtime->initialized = true;
    if (write_runtime_results(runtime) == 0)
        return 0;
    awavma_runtime_shutdown(runtime);
    return EIO;

fail:
    awavma_runtime_shutdown(runtime);
    return EIO;
}

int awavma_runtime_run_for(awavma_runtime_t *runtime, uint64_t duration_ms)
{
    uint64_t started;

    if (runtime == NULL || !runtime->initialized)
        return EINVAL;
    started = monotonic_ms();
    do {
        uint64_t slice = runtime->config.evaluation_interval_ms;

        if (duration_ms != 0 && monotonic_ms() - started < duration_ms &&
            duration_ms - (monotonic_ms() - started) < slice)
            slice = duration_ms - (monotonic_ms() - started);
        if (slice == 0)
            break;
        monitor_profile_scope_t cycle_profile;

        monitor_profile_scope_begin(&cycle_profile, "runtime", "runtime_cycle_total", NULL, -1, 0);
        if (runtime_monitor_run_for(runtime->monitor, slice) != 0) {
            monitor_profile_scope_end(&cycle_profile, "ERROR");
            return EIO;
        }
        if (process_pipeline(runtime) != 0) {
            monitor_profile_scope_end(&cycle_profile, "ERROR");
            return EIO;
        }
        monitor_profile_scope_end(&cycle_profile, "OK");
    } while (!atomic_load(&runtime->stop_requested) &&
             (duration_ms == 0 || monotonic_ms() - started < duration_ms));
    return 0;
}

void awavma_runtime_request_stop(awavma_runtime_t *runtime)
{
    if (runtime == NULL)
        return;
    atomic_store(&runtime->stop_requested, true);
    runtime_monitor_request_stop(runtime->monitor);
}

size_t awavma_runtime_snapshot(const awavma_runtime_t *runtime,
                               awavma_runtime_record_t *records, size_t capacity)
{
    size_t count;

    if (runtime == NULL || !runtime->initialized || records == NULL || capacity == 0)
        return 0;
    count = runtime->record_count < capacity ? runtime->record_count : capacity;
    memcpy(records, runtime->records, count * sizeof(*records));
    return count;
}

#ifdef AWAVMA_RUNTIME_TESTING
int awavma_runtime_test_submit_approved_migration(
    awavma_runtime_t *runtime, pid_t pid, uint64_t start_time_ticks,
    awavma_runtime_test_target_case_t target_case,
    awavma_runtime_test_target_stats_t *stats)
{
    MigrationSafetyRequest request = {0};
    MigrationSafetyResult result;

    if (runtime == NULL || !runtime->initialized || runtime->migration_safety == NULL ||
        stats == NULL)
        return EINVAL;
    memset(&runtime->test_target_stats, 0, sizeof(runtime->test_target_stats));
    runtime->test_target_case = target_case;
    snprintf(request.app_id, sizeof(request.app_id), "target-integration");
    request.pid = pid;
    request.start_time_ticks = start_time_ticks;
    request.action = target_case == AWAVMA_RUNTIME_TEST_TARGET_PAGE_ADDRESS_UNAVAILABLE ?
        VALIDATION_ACTION_MOVE_MEMORY : VALIDATION_ACTION_MOVE_THREAD;
    request.target_requires_cross_node = target_case == AWAVMA_RUNTIME_TEST_TARGET_NO_ALTERNATE;
    request.source_numa_node = 0;
    request.destination_numa_node = 0;
    request.placement_available = true;
    request.system_safe = true;
    request.migration_request.pid = pid;
    request.migration_request.start_time_ticks = start_time_ticks;
    request.migration_request.start_time_ticks_available = true;
    request.migration_request.tid = pid;
    request.migration_request.phase5_decision.action = request.action;
    request.migration_request.phase5_decision.pid = pid;
    request.migration_request.phase5_decision.evidence_model = DECISION_EVIDENCE_UTILITY_POLICY;
    request.migration_request.phase5_decision.phase5_evidence_available = true;
    request.migration_request.phase5_decision.phase5_utility_available = true;
    request.migration_request.phase5_decision.phase5_decision_margin_available = true;
    request.migration_request.phase5_decision.phase5_epsilon_available = true;
    request.migration_request.phase5_decision.phase5_versions_available = true;
    request.migration_request.phase5_decision.phase5_runtime_generation_available = true;
    request.migration_request.phase5_decision.phase5_runtime_generation = 1;
    request.migration_request.phase5_decision.phase5_memory_score_final = 0.0;
    request.migration_request.phase5_decision.phase5_thread_score_final = 0.24;
    request.migration_request.phase5_decision.phase5_decision_margin = -0.24;
    request.migration_request.phase5_decision.phase5_epsilon = 0.05;
    request.migration_request.phase5_decision.phase5_weight_version = 1;
    request.migration_request.phase5_decision.phase5_bias_version = 1;
    snprintf(request.migration_request.phase5_decision.phase5_decision_status,
             sizeof(request.migration_request.phase5_decision.phase5_decision_status), "DECISION_VALID");
    snprintf(request.migration_request.phase5_decision.migration_id,
             sizeof(request.migration_request.phase5_decision.migration_id), "test-migration-1");
    request.migration_request.benefit_evidence_runtime_generation = 1;
    request.migration_request.phase6_validation.pid = pid;
    request.migration_request.phase6_validation.action = request.action;
    request.migration_request.phase6_validation.confidence_status = GATE_PASS;
    request.migration_request.phase6_validation.roi_status = GATE_NOT_APPLICABLE;
    request.migration_request.phase6_validation.confidence_score = 80.0;
    request.migration_request.phase6_validation.roi_score = -1.0;
    request.migration_request.phase6_validation.safety_status = GATE_PASS;
    request.migration_request.phase6_validation.safety_score = 80.0;
    snprintf(request.migration_request.phase6_validation.migration_id,
             sizeof(request.migration_request.phase6_validation.migration_id), "test-migration-1");
    snprintf(request.migration_request.phase5_decision.entity_id,
             sizeof(request.migration_request.phase5_decision.entity_id), "%s", request.app_id);
    snprintf(request.migration_request.phase6_validation.final_decision,
             sizeof(request.migration_request.phase6_validation.final_decision), "APPROVED");
    if (!migration_safety_manager_attempt(runtime->migration_safety, &request, &result)) {
        return EIO;
    }
    snprintf(runtime->test_target_stats.attempt_id, sizeof(runtime->test_target_stats.attempt_id),
             "%s", result.attempt_id);
    runtime->test_target_stats.structural_validation_committed =
        result.state == MIGRATION_SAFETY_COMMITTED &&
        (result.validation == MIGRATION_SAFETY_STRUCTURALLY_VALID ||
         result.validation == MIGRATION_SAFETY_STRUCTURALLY_VALID_PROGRESS);
    *stats = runtime->test_target_stats;
    return 0;
}

int awavma_runtime_test_resume_page_recovery(
    awavma_runtime_t *runtime, const char *active_attempt_id,
    const MigrationPageCheckpoint *checkpoint,
    awavma_runtime_test_target_stats_t *stats)
{
    MigrationSafetyRequest request = {0};
    MigrationSafetyResult result;
    MigrationPageCheckpoint copy;

    if (runtime == NULL || active_attempt_id == NULL || checkpoint == NULL || stats == NULL ||
        !runtime->initialized || runtime->migration_safety == NULL || checkpoint->entries == NULL)
        return EINVAL;
    memset(&runtime->test_target_stats, 0, sizeof(runtime->test_target_stats));
    snprintf(request.app_id, sizeof(request.app_id), "target-integration");
    request.pid = checkpoint->pid;
    request.start_time_ticks = checkpoint->start_time_ticks;
    request.action = VALIDATION_ACTION_MOVE_MEMORY;
    request.placement_available = true;
    request.system_safe = true;
    request.migration_request.pid = checkpoint->pid;
    request.migration_request.start_time_ticks = checkpoint->start_time_ticks;
    request.migration_request.start_time_ticks_available = true;
    if (checkpoint->complete && strcmp(active_attempt_id, checkpoint->attempt_id) == 0) {
        copy = *checkpoint;
        copy.entries = calloc(checkpoint->requested_count, sizeof(*copy.entries));
        if (copy.entries == NULL) return ENOMEM;
        memcpy(copy.entries, checkpoint->entries, checkpoint->requested_count * sizeof(*copy.entries));
        release_page_rollback_checkpoint(runtime, runtime->page_rollback_checkpoint.attempt_id);
        runtime->page_rollback_checkpoint = copy;
        runtime->page_rollback_checkpoint_available = true;
    }
    if (!migration_safety_test_resume_page_recovery(runtime->migration_safety, &request, active_attempt_id,
                                                     checkpoint, &result))
        return EIO;
    snprintf(runtime->test_target_stats.attempt_id, sizeof(runtime->test_target_stats.attempt_id),
             "%s", result.attempt_id);
    *stats = runtime->test_target_stats;
    return 0;
}
#endif

void awavma_runtime_shutdown(awavma_runtime_t *runtime)
{
    if (runtime == NULL)
        return;
    if (runtime->monitor != NULL)
        runtime_monitor_shutdown(runtime->monitor);
    if (runtime->pool != NULL)
        worker_pool_shutdown(runtime->pool);
    release_migration_checkpoint(runtime, runtime->rollback_attempt_id);
    release_page_rollback_checkpoint(runtime, runtime->page_rollback_checkpoint.attempt_id);
    migration_safety_manager_destroy(runtime->migration_safety);
    runtime->migration_safety = NULL;
    if (runtime->migration_initialized) {
        Migration_Shutdown();
        runtime->migration_initialized = false;
    }
    if (runtime->manager != NULL)
        application_manager_shutdown(runtime->manager);
    if (runtime->runtime_results_path[0] != '\0')
        (void)write_runtime_results(runtime);
    runtime->initialized = false;
}

void awavma_runtime_destroy(awavma_runtime_t *runtime)
{
    if (runtime == NULL)
        return;
    awavma_runtime_shutdown(runtime);
    runtime_monitor_destroy(runtime->monitor);
    worker_pool_destroy(runtime->pool);
    application_manager_destroy(runtime->manager);
    free(runtime->records);
    free(runtime);
}
