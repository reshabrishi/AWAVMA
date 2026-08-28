#include "validation_log.h"
#include "monitor_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VALIDATION_HISTORY_HEADER "timestamp,migration_id,app_id,pid,entity_id,action,source_node,destination_node,confidence_score,roi_score,safety_score,validation_score,confidence_status,roi_status,safety_status,validation_status,final_decision,history_relevance,recorded_at_epoch"
#define VALIDATION_RESULTS_HEADER "timestamp,migration_id,app_id,pid,entity_id,action,source_node,destination_node,confidence_score,roi_score,safety_score,validation_score,confidence_status,roi_status,safety_status,validation_status,final_decision"

static ValidationConfig log_config;
static bool log_initialized;
static size_t log_count;

static const char *gate_name(GateStatus status)
{
    return status == GATE_PASS ? "PASS" : status == GATE_FAIL ? "FAIL" : "INVALID";
}

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

static int append_line(const char *path, const char *header, const char *line)
{
    FILE *file = fopen(path, "a+");

    if (file == NULL)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    if (ftell(file) == 0)
        fprintf(file, "%s\n", header);
    fprintf(file, "%s\n", line);
    if (fflush(file) != 0) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static int compact_history(void)
{
    FILE *input;
    FILE *output;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    char *header = NULL;
    char **records = NULL;
    size_t count = 0;
    long long now = epoch_seconds();
    long long max_age = (long long)(log_config.history_max_days * 86400.0);
    char temporary[512];

    input = fopen(log_config.history_path, "r");
    if (input == NULL && errno == ENOENT)
        return 0;
    if (input == NULL)
        return -1;
    length = getline(&line, &capacity, input);
    if (length < 0) {
        fclose(input);
        free(line);
        return -1;
    }
    header = strdup(line);
    while ((length = getline(&line, &capacity, input)) >= 0) {
        char *last_comma = strrchr(line, ',');
        long long recorded = last_comma != NULL ? strtoll(last_comma + 1, NULL, 10) : now;
        char *copy;
        char **expanded;

        if (recorded > 0 && now - recorded > max_age)
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
    if (count > log_config.history_max_records) {
        size_t first = count - log_config.history_max_records;
        for (size_t index = 0; index < first; index++)
            free(records[index]);
        memmove(records, records + first, log_config.history_max_records * sizeof(*records));
        count = log_config.history_max_records;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", log_config.history_path) >= (int)sizeof(temporary))
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
        if (previous_comma == NULL || *previous_comma != ',') {
            fputs(records[index], output);
            continue;
        }
        fwrite(records[index], 1, (size_t)(previous_comma - records[index] + 1), output);
        fprintf(output, "%.9f%s", exp(-log_config.history_decay_lambda * age_days), last_comma);
    }
    if (fflush(output) != 0 || fsync(fileno(output)) != 0 || fclose(output) != 0) {
        unlink(temporary);
        goto fail;
    }
    if (rename(temporary, log_config.history_path) != 0)
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
    for (size_t index = 0; index < count; index++)
        free(records[index]);
    free(records);
    free(header);
    free(line);
    return -1;
}

bool validation_log_init(const ValidationConfig *config)
{
    if (config == NULL || config->results_path == NULL || config->history_path == NULL || config->log_path == NULL)
        return false;
    log_config = *config;
    log_initialized = true;
    log_count = 0;
    return true;
}

bool LogValidationResult(const ValidationResult *result, const DecisionData *decision)
{
    char line[2048];
    char history_line[2200];
    long long now;
    int source_node;
    int destination_node;
    const char *timestamp;
    FILE *human;

    if (!log_initialized || result == NULL || decision == NULL)
        return false;
    timestamp = result->timestamp[0] && strcmp(result->timestamp, "UNKNOWN") != 0
                    ? result->timestamp : "UNKNOWN";
    source_node = decision->nodes_available ? decision->source_node : -1;
    destination_node = decision->nodes_available ? decision->destination_node : -1;
    snprintf(line, sizeof(line), "%s,%s,%s,%ld,%s,%s,%d,%d,%.9f,%.9f,%.9f,%.9f,%s,%s,%s,%s,%s",
             timestamp, result->migration_id, result->app_id, result->pid, result->entity_id,
             action_name(result->action), source_node, destination_node,
             result->confidence_score, result->roi_score, result->safety_score,
             result->validation_score, gate_name(result->confidence_status),
             gate_name(result->roi_status), gate_name(result->safety_status),
             result->validation_status, result->final_decision);
    now = epoch_seconds();
    snprintf(history_line, sizeof(history_line), "%s,1.000000000,%lld", line, now);
    monitor_profile_scope_t output_profile;
    monitor_profile_scope_begin(&output_profile, "phase56_child", "p6_output_history_write",
                                result->app_id, result->pid, 0);
    if (append_line(log_config.results_path, VALIDATION_RESULTS_HEADER, line) != 0 ||
        append_line(log_config.history_path, VALIDATION_HISTORY_HEADER, history_line) != 0) {
        monitor_profile_scope_end(&output_profile, "ERROR");
        return false;
    }
    monitor_profile_scope_end(&output_profile, "OK");
    monitor_profile_scope_t human_log_profile;
    monitor_profile_scope_begin(&human_log_profile, "phase56_child", "p6_human_log_write",
                                result->app_id, result->pid, 0);
    human = fopen(log_config.log_path, "a");
    if (human != NULL) {
        fprintf(human, "%lld app_id=%s pid=%ld action=%s status=%s decision=%s\n",
                now, result->app_id, result->pid, action_name(result->action),
                result->validation_status, result->final_decision);
        fclose(human);
    }
    monitor_profile_scope_end(&human_log_profile, human != NULL ? "OK" : "SKIPPED");
    log_count++;
    if (log_count % log_config.history_cleanup_interval == 0) {
        monitor_profile_scope_t cleanup_profile;

        monitor_profile_scope_begin(&cleanup_profile, "phase56_child", "p6_history_cleanup",
                                    result->app_id, result->pid, 0);
        compact_history();
        monitor_profile_scope_end(&cleanup_profile, "OK");
    }
    return true;
}

void validation_log_shutdown(void)
{
    log_initialized = false;
    memset(&log_config, 0, sizeof(log_config));
}
