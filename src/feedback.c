#include "feedback.h"
#include "feedback_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FeedbackConfig active_config;
static bool initialized;

static double monotonic_seconds(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

const char *FeedbackClassName(FeedbackClass feedback_class)
{
    switch (feedback_class) {
    case FEEDBACK_STRONGLY_POSITIVE: return "FEEDBACK_STRONGLY_POSITIVE";
    case FEEDBACK_POSITIVE: return "FEEDBACK_POSITIVE";
    case FEEDBACK_NEUTRAL: return "FEEDBACK_NEUTRAL";
    case FEEDBACK_NEGATIVE: return "FEEDBACK_NEGATIVE";
    case FEEDBACK_STRONGLY_NEGATIVE: return "FEEDBACK_STRONGLY_NEGATIVE";
    case FEEDBACK_NOT_LEARNABLE: return "FEEDBACK_NOT_LEARNABLE";
    case FEEDBACK_INSUFFICIENT_OBSERVATION: return "FEEDBACK_INSUFFICIENT_OBSERVATION";
    case FEEDBACK_INVALID_INPUT: return "FEEDBACK_INVALID_INPUT";
    case FEEDBACK_INVALID_STATE: return "FEEDBACK_INVALID_STATE";
    case FEEDBACK_NO_UPDATE: return "FEEDBACK_NO_UPDATE";
    default: return "FEEDBACK_INVALID_INPUT";
    }
}

const char *FeedbackStatusName(FeedbackUpdateStatus status)
{
    switch (status) {
    case FEEDBACK_UPDATED: return "FEEDBACK_UPDATED";
    case FEEDBACK_UPDATE_NOT_LEARNABLE: return "FEEDBACK_NOT_LEARNABLE";
    case FEEDBACK_UPDATE_INSUFFICIENT_OBSERVATION: return "FEEDBACK_INSUFFICIENT_OBSERVATION";
    case FEEDBACK_UPDATE_NO_UPDATE: return "FEEDBACK_NO_UPDATE";
    case FEEDBACK_UPDATE_INVALID_STATE: return "FEEDBACK_INVALID_STATE";
    case FEEDBACK_UPDATE_INVALID_INPUT: return "FEEDBACK_INVALID_INPUT";
    default: return "FEEDBACK_INVALID_INPUT";
    }
}

static bool valid_config(const FeedbackConfig *config)
{
    double metric_weight_sum;

    if (config == NULL || config->state_dir == NULL || config->history_dir == NULL ||
        config->history_path == NULL || config->log_path == NULL || config->min_samples == 0 ||
        !isfinite(config->learning_rate) || !isfinite(config->bias_learning_rate) ||
        !isfinite(config->deadband) || !isfinite(config->reward_min) || !isfinite(config->reward_max) ||
        !isfinite(config->decay_lambda) || !isfinite(config->min_confidence) ||
        !isfinite(config->history_max_days) || !isfinite(config->min_weight) ||
        !isfinite(config->max_weight) || !isfinite(config->min_bias) || !isfinite(config->max_bias) ||
        !isfinite(config->throughput_weight) || !isfinite(config->execution_time_weight) ||
        !isfinite(config->latency_weight) || !isfinite(config->page_fault_weight))
        return false;
    metric_weight_sum = config->throughput_weight + config->execution_time_weight +
                        config->latency_weight + config->page_fault_weight;
    return config->learning_rate >= 0.0 && config->bias_learning_rate >= 0.0 &&
           config->deadband >= 0.0 && config->reward_min >= -1.0 && config->reward_max <= 1.0 &&
           config->reward_min < config->reward_max && config->decay_lambda > 0.0 &&
           config->min_confidence >= 0.0 && config->min_confidence <= 1.0 &&
           config->history_max_records > 0 && config->history_max_days > 0.0 &&
           config->cleanup_interval > 0 && config->min_weight >= 0.0 &&
           config->max_weight >= config->min_weight && config->min_bias <= config->max_bias &&
           metric_weight_sum > 0.0 && config->throughput_weight >= 0.0 &&
           config->execution_time_weight >= 0.0 && config->latency_weight >= 0.0 &&
           config->page_fault_weight >= 0.0;
}

bool Feedback_Init(const FeedbackConfig *config)
{
    if (!valid_config(config) || !feedback_log_init(config))
        return false;
    active_config = *config;
    initialized = true;
    return true;
}

void Feedback_Shutdown(void)
{
    feedback_log_shutdown();
    initialized = false;
    memset(&active_config, 0, sizeof(active_config));
}

static void result_init(FeedbackResult *result)
{
    memset(result, 0, sizeof(*result));
    result->feedback_class = FEEDBACK_INVALID_INPUT;
    result->update_status = FEEDBACK_UPDATE_INVALID_INPUT;
}

static bool valid_identifier(const char *value)
{
    return value != NULL && value[0] != '\0' && strlen(value) < 128U &&
           strchr(value, ',') == NULL && strchr(value, '\n') == NULL;
}

static bool nonlearnable_migration_result(const char *result)
{
    return result == NULL || (strcmp(result, "MIGRATION_SUCCESS") != 0 &&
                              strcmp(result, "MIGRATION_PARTIAL_SUCCESS") != 0);
}

static double clamp_reward(double value)
{
    if (value < active_config.reward_min)
        return active_config.reward_min;
    if (value > active_config.reward_max)
        return active_config.reward_max;
    return value;
}

static double metric_weight(FeedbackMetric metric)
{
    switch (metric) {
    case FEEDBACK_METRIC_THROUGHPUT: return active_config.throughput_weight;
    case FEEDBACK_METRIC_EXECUTION_TIME: return active_config.execution_time_weight;
    case FEEDBACK_METRIC_LATENCY: return active_config.latency_weight;
    case FEEDBACK_METRIC_PAGE_FAULTS: return active_config.page_fault_weight;
    default: return 0.0;
    }
}

static double metric_improvement(FeedbackMetric metric, double before, double after)
{
    const double epsilon = 1.0e-9;
    double denominator = fmax(fabs(before), epsilon);
    double improvement = metric == FEEDBACK_METRIC_THROUGHPUT
                             ? (after - before) / denominator
                             : (before - after) / denominator;

    if (improvement < active_config.reward_min)
        return active_config.reward_min;
    if (improvement > active_config.reward_max)
        return active_config.reward_max;
    return improvement;
}

static double migration_cost_penalty(const FeedbackEvent *event)
{
    double normalized_cost;

    if (event == NULL || !event->migration_cost_available ||
        !isfinite(event->migration_execution_time_ms) || event->migration_execution_time_ms < 0.0)
        return 0.0;
    if (event->metric_available[FEEDBACK_METRIC_EXECUTION_TIME] &&
        event->before_metrics[FEEDBACK_METRIC_EXECUTION_TIME] > 1.0e-9) {
        normalized_cost = event->migration_execution_time_ms /
                          event->before_metrics[FEEDBACK_METRIC_EXECUTION_TIME];
    } else if (event->pages_migrated + event->pages_failed > 0) {
        normalized_cost = (double)event->pages_failed /
                          (double)(event->pages_migrated + event->pages_failed);
    } else {
        return 0.0;
    }
    if (normalized_cost < 0.0)
        normalized_cost = 0.0;
    if (normalized_cost > 1.0)
        normalized_cost = 1.0;
    return 0.10 * normalized_cost;
}

static double historical_relevance(const FeedbackEvent *event)
{
    long long now;
    double age_days;

    if (!event->epoch_available || event->recorded_at_epoch <= 0)
        return 1.0;
    now = (long long)time(NULL);
    age_days = now > event->recorded_at_epoch
                   ? (double)(now - event->recorded_at_epoch) / 86400.0 : 0.0;
    return exp(-active_config.decay_lambda * age_days);
}

static bool valid_event_values(const FeedbackEvent *event)
{
    if (event == NULL || !valid_identifier(event->app_id) ||
        !valid_identifier(event->migration_id) || !valid_identifier(event->feedback_id))
        return false;
    if (event->sample_counts_available && (event->before_samples == 0 || event->after_samples == 0))
        return false;
    if (event->supplied_confidence_available &&
        (!isfinite(event->supplied_confidence) || event->supplied_confidence < 0.0 ||
         event->supplied_confidence > 1.0))
        return false;
    if (event->migration_cost_available &&
        (!isfinite(event->migration_execution_time_ms) || event->migration_execution_time_ms < 0.0))
        return false;
    for (size_t index = 0; index < FEEDBACK_SIGNAL_COUNT; index++)
        if (event->factor_available[index] &&
            (!isfinite(event->factor_signals[index]) || event->factor_signals[index] < 0.0 ||
             event->factor_signals[index] > 1.0))
            return false;
    return true;
}

static FeedbackClass classify_reward(double reward)
{
    if (fabs(reward) < active_config.deadband)
        return FEEDBACK_NEUTRAL;
    if (reward >= 0.5)
        return FEEDBACK_STRONGLY_POSITIVE;
    if (reward > 0.0)
        return FEEDBACK_POSITIVE;
    if (reward <= -0.5)
        return FEEDBACK_STRONGLY_NEGATIVE;
    return FEEDBACK_NEGATIVE;
}

FeedbackUpdateStatus ProcessFeedback(const FeedbackEvent *event, FeedbackResult *result)
{
    double reward_sum = 0.0;
    double weight_sum = 0.0;
    double sample_confidence;
    double metric_confidence;
    double relevance;
    double start_time;
    int offset;

#define RETURN_FEEDBACK(status_value) do { \
        result->processing_time_ms = (monotonic_seconds() - start_time) * 1000.0; \
        feedback_log_result(event, result); \
        return (status_value); \
    } while (0)

    if (result == NULL)
        return FEEDBACK_UPDATE_INVALID_INPUT;
    start_time = monotonic_seconds();
    result_init(result);
    if (!initialized || event == NULL) {
        result->feedback_class = FEEDBACK_INVALID_INPUT;
        snprintf(result->reason, sizeof(result->reason), "event is invalid or module is not initialized");
        result->update_status = FEEDBACK_UPDATE_INVALID_INPUT;
        RETURN_FEEDBACK(result->update_status);
    }
    if (event->action == VALIDATION_ACTION_NO_MIGRATION || event->action == VALIDATION_ACTION_INSUFFICIENT) {
        result->feedback_class = FEEDBACK_NO_UPDATE;
        result->update_status = FEEDBACK_UPDATE_NO_UPDATE;
        snprintf(result->reason, sizeof(result->reason), "no migration experiment was executed");
        RETURN_FEEDBACK(result->update_status);
    }
    if ((event->action != VALIDATION_ACTION_MOVE_MEMORY &&
         event->action != VALIDATION_ACTION_MOVE_THREAD) ||
        strcmp(event->phase6_validation, "APPROVED") != 0 ||
        nonlearnable_migration_result(event->migration_result)) {
        result->feedback_class = FEEDBACK_NOT_LEARNABLE;
        result->update_status = FEEDBACK_UPDATE_NOT_LEARNABLE;
        snprintf(result->reason, sizeof(result->reason),
                 "migration result is not a learnable execution outcome: %s",
                 event->migration_result[0] != '\0' ? event->migration_result : "missing");
        RETURN_FEEDBACK(result->update_status);
    }
    if (!valid_event_values(event)) {
        result->feedback_class = FEEDBACK_INVALID_INPUT;
        result->update_status = FEEDBACK_UPDATE_INVALID_INPUT;
        snprintf(result->reason, sizeof(result->reason), "event contains invalid values");
        RETURN_FEEDBACK(result->update_status);
    }
    if (!event->sample_counts_available || event->before_samples < active_config.min_samples ||
        event->after_samples < active_config.min_samples) {
        result->feedback_class = FEEDBACK_INSUFFICIENT_OBSERVATION;
        result->update_status = FEEDBACK_UPDATE_INSUFFICIENT_OBSERVATION;
        snprintf(result->reason, sizeof(result->reason),
                 "before/after observation counts are below feedback_min_samples");
        RETURN_FEEDBACK(result->update_status);
    }
    for (size_t metric = 0; metric < FEEDBACK_METRIC_COUNT; metric++) {
        double weight;

        if (!event->metric_available[metric])
            continue;
        if (!isfinite(event->before_metrics[metric]) || !isfinite(event->after_metrics[metric])) {
            result->feedback_class = FEEDBACK_INVALID_INPUT;
            result->update_status = FEEDBACK_UPDATE_INVALID_INPUT;
            snprintf(result->reason, sizeof(result->reason), "before/after metric is invalid");
            RETURN_FEEDBACK(result->update_status);
        }
        if (event->before_metrics[metric] == -1.0 || event->after_metrics[metric] == -1.0)
            continue;
        if (event->before_metrics[metric] < 0.0 || event->after_metrics[metric] < 0.0) {
            result->feedback_class = FEEDBACK_INVALID_INPUT;
            result->update_status = FEEDBACK_UPDATE_INVALID_INPUT;
            snprintf(result->reason, sizeof(result->reason), "before/after metric is negative");
            RETURN_FEEDBACK(result->update_status);
        }
        weight = metric_weight((FeedbackMetric)metric);
        reward_sum += weight * metric_improvement((FeedbackMetric)metric,
                                                   event->before_metrics[metric],
                                                   event->after_metrics[metric]);
        weight_sum += weight;
        result->metrics_used++;
    }
    if (result->metrics_used == 0 || weight_sum <= 0.0) {
        result->feedback_class = FEEDBACK_INSUFFICIENT_OBSERVATION;
        result->update_status = FEEDBACK_UPDATE_INSUFFICIENT_OBSERVATION;
        snprintf(result->reason, sizeof(result->reason), "no comparable before/after metrics are available");
        RETURN_FEEDBACK(result->update_status);
    }
    result->raw_reward = clamp_reward((reward_sum / weight_sum) - migration_cost_penalty(event));
    result->feedback_class = classify_reward(result->raw_reward);
    relevance = historical_relevance(event);
    result->historical_relevance = relevance;
    sample_confidence = fmin((double)fmin(event->before_samples, event->after_samples) /
                             (double)active_config.min_samples, 1.0);
    metric_confidence = (double)result->metrics_used / (double)FEEDBACK_METRIC_COUNT;
    result->feedback_confidence = sample_confidence * metric_confidence;
    if (event->verification_available)
        result->feedback_confidence *= event->verification_succeeded ? 1.0 : 0.0;
    else if (event->pages_migrated == 0 || event->pages_failed > 0)
        result->feedback_confidence *= 0.5;
    if (event->supplied_confidence_available)
        result->feedback_confidence *= event->supplied_confidence;
    if (result->feedback_confidence < 0.0)
        result->feedback_confidence = 0.0;
    if (result->feedback_confidence > 1.0)
        result->feedback_confidence = 1.0;
    result->effective_reward = result->raw_reward * result->feedback_confidence * relevance;
    if (fabs(result->raw_reward) < active_config.deadband) {
        result->feedback_class = FEEDBACK_NO_UPDATE;
        result->update_status = FEEDBACK_UPDATE_NO_UPDATE;
        snprintf(result->reason, sizeof(result->reason), "reward is inside feedback_deadband");
        RETURN_FEEDBACK(result->update_status);
    }
    if (result->feedback_confidence < active_config.min_confidence) {
        result->feedback_class = FEEDBACK_NO_UPDATE;
        result->update_status = FEEDBACK_UPDATE_NO_UPDATE;
        snprintf(result->reason, sizeof(result->reason), "feedback confidence is below minimum");
        RETURN_FEEDBACK(result->update_status);
    }
    memset(&result->applied_update, 0, sizeof(result->applied_update));
    offset = event->action == VALIDATION_ACTION_MOVE_MEMORY ? 0 : DECISION_FACTOR_COUNT;
    for (size_t factor = 0; factor < DECISION_FACTOR_COUNT; factor++) {
        if (!event->factor_available[offset + factor])
            continue;
        if (event->action == VALIDATION_ACTION_MOVE_MEMORY)
            result->applied_update.memory_delta[factor] = active_config.learning_rate *
                result->effective_reward * event->factor_signals[offset + factor];
        else
            result->applied_update.thread_delta[factor] = active_config.learning_rate *
                result->effective_reward * event->factor_signals[offset + factor];
    }
    if (event->action == VALIDATION_ACTION_MOVE_MEMORY)
        result->applied_update.memory_bias_delta = active_config.bias_learning_rate * result->effective_reward;
    else
        result->applied_update.thread_bias_delta = active_config.bias_learning_rate * result->effective_reward;
    {
        int apply_result = decision_feedback_apply(&(decision_config_t){
            .state_dir = active_config.state_dir,
            .history_dir = active_config.history_dir,
            .bias_min = active_config.min_bias,
            .bias_max = active_config.max_bias
        }, event->app_id, &result->applied_update, active_config.min_weight,
        active_config.max_weight, &result->state);

        if (apply_result == -2) {
            result->feedback_class = FEEDBACK_NO_UPDATE;
            result->update_status = FEEDBACK_UPDATE_NO_UPDATE;
            snprintf(result->reason, sizeof(result->reason), "application is EXPIRED and was not reactivated");
            RETURN_FEEDBACK(result->update_status);
        }
        if (apply_result != 0) {
            result->feedback_class = FEEDBACK_INVALID_STATE;
            result->update_status = FEEDBACK_UPDATE_INVALID_STATE;
            snprintf(result->reason, sizeof(result->reason),
                     "Phase 5 weight or bias state is invalid or could not be persisted");
            RETURN_FEEDBACK(result->update_status);
        }
    }
    result->update_status = FEEDBACK_UPDATED;
    snprintf(result->reason, sizeof(result->reason),
             "observed before/after outcome updated selected application state");
    RETURN_FEEDBACK(result->update_status);
#undef RETURN_FEEDBACK
}

bool Feedback_CleanupHistory(void)
{
    return initialized && feedback_log_cleanup();
}
