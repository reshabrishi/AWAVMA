#include "feedback_log.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FeedbackConfig active_config;
static bool initialized;
static size_t event_count;

static const char *action_name(ValidationAction action)
{
    switch (action) {
    case VALIDATION_ACTION_MOVE_MEMORY: return "MOVE_MEMORY";
    case VALIDATION_ACTION_MOVE_THREAD: return "MOVE_THREAD";
    case VALIDATION_ACTION_NO_MIGRATION: return "NO_MIGRATION";
    default: return "INSUFFICIENT_DECISION_SIGNAL";
    }
}

static long long epoch_seconds(void)
{
    return (long long)time(NULL);
}

static void write_header(FILE *file)
{
    fputs("timestamp,feedback_id,migration_id,app_id,pid,entity_id,action,phase6_validation,migration_result,", file);
    fputs("before_throughput,after_throughput,before_execution_time,after_execution_time,before_latency,after_latency,before_page_faults,after_page_faults,", file);
    fputs("before_samples,after_samples,raw_reward,effective_reward,feedback_confidence,historical_relevance,metrics_used,processing_time_ms,feedback_class,update_status,reason,", file);
    for (int action = 0; action < 2; action++)
        for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++) {
            const char *prefix = action == DECISION_MEMORY ? "memory" : "thread";
            fprintf(file, "old_%s_%s,signal_%s_%s,reward_%s_%s,confidence_%s_%s,relevance_%s_%s,effective_reward_%s_%s,learning_rate_%s_%s,delta_%s_%s,new_%s_%s,",
                    prefix, decision_factor_name((decision_factor_t)factor),
                    prefix, decision_factor_name((decision_factor_t)factor),
                    prefix, decision_factor_name((decision_factor_t)factor),
                    prefix, decision_factor_name((decision_factor_t)factor),
                    prefix, decision_factor_name((decision_factor_t)factor),
                    prefix, decision_factor_name((decision_factor_t)factor),
                    prefix, decision_factor_name((decision_factor_t)factor),
                    prefix, decision_factor_name((decision_factor_t)factor),
                    prefix, decision_factor_name((decision_factor_t)factor));
        }
    fputs("old_memory_bias,effective_reward_memory_bias,learning_rate_memory_bias,delta_memory_bias,new_memory_bias,old_thread_bias,effective_reward_thread_bias,learning_rate_thread_bias,delta_thread_bias,new_thread_bias,history_relevance_epoch\n", file);
}

static void write_metric(FILE *file, const FeedbackEvent *event, FeedbackMetric metric, bool before)
{
    if (event == NULL || !event->metric_available[metric])
        fputs("NA", file);
    else
        fprintf(file, "%.9f", before ? event->before_metrics[metric] : event->after_metrics[metric]);
}

static int append_record(const char *path, const FeedbackEvent *event, const FeedbackResult *result)
{
    FILE *file = fopen(path, "a+");
    long position;
    long long now = epoch_seconds();

    if (file == NULL)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0 || (position = ftell(file)) < 0) {
        fclose(file);
        return -1;
    }
    if (position == 0)
        write_header(file);
    fprintf(file, "%s,%s,%s,%s,%ld,%s,%s,%s,%s,",
            event != NULL ? event->timestamp : "UNKNOWN",
            event != NULL ? event->feedback_id : "UNKNOWN",
            event != NULL ? event->migration_id : "UNKNOWN",
            event != NULL ? event->app_id : "UNKNOWN",
            event != NULL ? event->pid : 0L,
            event != NULL ? event->entity_id : "UNKNOWN",
            event != NULL ? action_name(event->action) : "UNKNOWN",
            event != NULL ? event->phase6_validation : "UNKNOWN",
            event != NULL ? event->migration_result : "UNKNOWN");
    for (int metric = 0; metric < (int)FEEDBACK_METRIC_COUNT; metric++) {
        write_metric(file, event, (FeedbackMetric)metric, true);
        fputc(',', file);
        write_metric(file, event, (FeedbackMetric)metric, false);
        fputc(',', file);
    }
    fprintf(file, "%zu,%zu,%.9f,%.9f,%.9f,%.9f,%zu,%.6f,%s,%s,%s,",
            event != NULL ? event->before_samples : 0,
            event != NULL ? event->after_samples : 0,
            result->raw_reward, result->effective_reward, result->feedback_confidence,
            result->historical_relevance, result->metrics_used, result->processing_time_ms,
            FeedbackClassName(result->feedback_class), FeedbackStatusName(result->update_status), result->reason);
    for (int action = 0; action < 2; action++)
        for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++) {
            size_t signal_index = (size_t)(action * DECISION_FACTOR_COUNT + factor);
            double old_value = action == DECISION_MEMORY ? result->state.old_memory_weights[factor] : result->state.old_thread_weights[factor];
            double new_value = action == DECISION_MEMORY ? result->state.new_memory_weights[factor] : result->state.new_thread_weights[factor];
            double delta = action == DECISION_MEMORY ? result->applied_update.memory_delta[factor] : result->applied_update.thread_delta[factor];

            fprintf(file, "%.9f,", old_value);
            if (event == NULL || !event->factor_available[signal_index])
                fputs("NA,", file);
            else
                fprintf(file, "%.9f,", event->factor_signals[signal_index]);
            fprintf(file, "%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,",
                    result->raw_reward, result->feedback_confidence,
                    result->historical_relevance, result->effective_reward,
                    action == DECISION_MEMORY ? active_config.learning_rate : active_config.learning_rate,
                    delta, new_value);
        }
    fprintf(file, "%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%lld\n",
            result->state.old_memory_bias, result->effective_reward, active_config.bias_learning_rate,
            result->applied_update.memory_bias_delta, result->state.new_memory_bias,
            result->state.old_thread_bias, result->effective_reward, active_config.bias_learning_rate,
            result->applied_update.thread_bias_delta, result->state.new_thread_bias, now);
    if (fflush(file) != 0) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0)
        return -1;
    return 0;
}

static int compact_history(void)
{
    FILE *input = NULL;
    FILE *output = NULL;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    char *header = NULL;
    char **records = NULL;
    size_t count = 0;
    long long now = epoch_seconds();
    long long max_age = (long long)(active_config.history_max_days * 86400.0);
    char temporary[512] = {0};

    input = fopen(active_config.history_path, "r");
    if (input == NULL && errno == ENOENT)
        return 0;
    if (input == NULL)
        return -1;
    length = getline(&line, &capacity, input);
    if (length < 0)
        goto fail;
    header = strdup(line);
    if (header == NULL)
        goto fail;
    while ((length = getline(&line, &capacity, input)) >= 0) {
        char *last_comma = strrchr(line, ',');
        long long recorded = last_comma != NULL ? strtoll(last_comma + 1, NULL, 10) : now;
        char *copy;
        char **expanded;

        if (recorded > 0 && now >= recorded && now - recorded > max_age)
            continue;
        copy = strdup(line);
        if (copy == NULL)
            goto fail;
        expanded = realloc(records, (count + 1) * sizeof(*records));
        if (expanded == NULL) {
            free(copy);
            goto fail;
        }
        records = expanded;
        records[count++] = copy;
    }
    fclose(input);
    input = NULL;
    if (count > active_config.history_max_records) {
        size_t first = count - active_config.history_max_records;

        for (size_t index = 0; index < first; index++)
            free(records[index]);
        memmove(records, records + first, active_config.history_max_records * sizeof(*records));
        count = active_config.history_max_records;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", active_config.history_path) >= (int)sizeof(temporary))
        goto fail;
    output = fopen(temporary, "w");
    if (output == NULL)
        goto fail;
    fputs(header, output);
    for (size_t index = 0; index < count; index++) {
        char *last_comma = strrchr(records[index], ',');
        char *previous_comma = last_comma != NULL ? last_comma - 1 : NULL;
        long long recorded = last_comma != NULL ? strtoll(last_comma + 1, NULL, 10) : now;
        double age_days = recorded > 0 && now >= recorded ? (double)(now - recorded) / 86400.0 : 0.0;

        while (previous_comma != NULL && previous_comma > records[index] && *previous_comma != ',')
            previous_comma--;
        if (previous_comma == NULL || *previous_comma != ',')
            fputs(records[index], output);
        else {
            fwrite(records[index], 1, (size_t)(previous_comma - records[index] + 1), output);
            fprintf(output, "%.9f%s", exp(-active_config.decay_lambda * age_days), last_comma);
        }
    }
    if (fflush(output) != 0 || fclose(output) != 0) {
        output = NULL;
        goto fail;
    }
    output = NULL;
    if (rename(temporary, active_config.history_path) != 0)
        goto fail;
    for (size_t index = 0; index < count; index++)
        free(records[index]);
    free(records);
    free(header);
    free(line);
    return 0;

fail:
    if (input != NULL)
        fclose(input);
    if (output != NULL)
        fclose(output);
    if (temporary[0] != '\0')
        unlink(temporary);
    for (size_t index = 0; index < count; index++)
        free(records[index]);
    free(records);
    free(header);
    free(line);
    return -1;
}

bool feedback_log_init(const FeedbackConfig *config)
{
    if (config == NULL || config->history_path == NULL || config->log_path == NULL ||
        config->history_max_records == 0 || config->history_max_days <= 0.0 ||
        config->cleanup_interval == 0 || config->decay_lambda <= 0.0 || !isfinite(config->decay_lambda))
        return false;
    active_config = *config;
    initialized = true;
    event_count = 0;
    return true;
}

bool feedback_log_result(const FeedbackEvent *event, const FeedbackResult *result)
{
    FILE *human;
    long long now;

    if (!initialized || result == NULL)
        return false;
    if (append_record(active_config.history_path, event, result) != 0)
        return false;
    if (active_config.results_path != NULL &&
        append_record(active_config.results_path, event, result) != 0)
        return false;
    human = fopen(active_config.log_path, "a");
    if (human == NULL)
        return false;
    now = epoch_seconds();
    fprintf(human, "%lld feedback_id=%s migration_id=%s app_id=%s action=%s reward=%.6f confidence=%.6f class=%s status=%s reason=%s\n",
            now, event != NULL ? event->feedback_id : "UNKNOWN", event != NULL ? event->migration_id : "UNKNOWN",
            event != NULL ? event->app_id : "UNKNOWN", event != NULL ? action_name(event->action) : "UNKNOWN",
            result->effective_reward, result->feedback_confidence, FeedbackClassName(result->feedback_class),
            FeedbackStatusName(result->update_status), result->reason);
    if (fclose(human) != 0)
        return false;
    event_count++;
    if (event_count % active_config.cleanup_interval == 0)
        compact_history();
    return true;
}

bool feedback_log_cleanup(void)
{
    return initialized && compact_history() == 0;
}

void feedback_log_shutdown(void)
{
    initialized = false;
    memset(&active_config, 0, sizeof(active_config));
    event_count = 0;
}
