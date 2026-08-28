#include "decision.h"
#include "monitor_profile.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GLOBAL_APP_ID "__GLOBAL_DEFAULTS__"
#define DEFAULT_ENTITY_ID "workload"
#define MAX_CSV_COLUMNS 128U
#define MAX_ID_LENGTH 127U
#define HISTORY_DECISION_HEADER "timestamp,app_id,pid,entity_id,classification,classification_score,f_access,f_threshold,f_gain_memory,f_cost_memory,f_cpu_memory,f_sharing_memory,f_gain_thread,f_cost_thread,f_cpu_thread,f_sharing_thread,memory_score_raw,thread_score_raw,memory_bias,thread_bias,memory_score_final,thread_score_final,epsilon,decision,weight_version,bias_version,status,history_relevance,recorded_at_epoch"
#define HISTORY_WEIGHT_HEADER "app_id,action,factor,old_weight,new_weight,version,updated_at,history_relevance,recorded_at_epoch"

static const double default_memory_weights[DECISION_FACTOR_COUNT] = {
    1.0, 0.8, 1.2, 1.0, 0.4, 0.6
};
static const double default_thread_weights[DECISION_FACTOR_COUNT] = {
    1.0, 0.8, 1.2, 1.0, 0.8, 0.5
};

typedef struct {
    char *app_id;
    decision_action_t action;
    decision_factor_t factor;
    double weight;
    unsigned version;
    char updated_at[32];
} weight_record_t;

typedef struct {
    char *app_id;
    double memory_bias;
    double thread_bias;
    unsigned version;
    char updated_at[32];
} bias_record_t;

typedef struct {
    char *app_id;
    long pid;
    size_t entity_count;
    size_t hot_count;
    size_t moderate_count;
    size_t cold_count;
    unsigned profile_version;
    char last_seen[128];
    char status[16];
} application_record_t;

typedef struct {
    char *id;
    double memory_weights[DECISION_FACTOR_COUNT];
    double thread_weights[DECISION_FACTOR_COUNT];
    double memory_bias;
    double thread_bias;
    unsigned weight_version;
    unsigned bias_version;
    long last_pid;
    char last_seen[128];
    size_t entity_count;
    size_t hot_count;
    size_t moderate_count;
    size_t cold_count;
    char **entities;
    size_t entities_count;
} application_context_t;

typedef struct {
    int timestamp;
    int elapsed_ms;
    int pid;
    int app_id;
    int entity_id;
    int classification;
    int score;
    int factors[10];
    size_t count;
} decision_columns_t;

typedef struct {
    double values[10];
    bool available[10];
    char missing[256];
} factor_values_t;

typedef struct {
    decision_action_t action;
    double raw_memory;
    double raw_thread;
    double final_memory;
    double final_thread;
    const char *decision;
} decision_result_t;

static const char *factor_names[] = {
    "ACCESS", "THRESHOLD", "GAIN", "COST", "CPU", "SHARING"
};

static const char *output_header(void)
{
    return "timestamp,app_id,pid,entity_id,classification,classification_score,f_access,f_threshold,f_gain_memory,f_cost_memory,f_cpu_memory,f_sharing_memory,f_gain_thread,f_cost_thread,f_cpu_thread,f_sharing_thread,memory_score_raw,thread_score_raw,memory_bias,thread_bias,memory_score_final,thread_score_final,epsilon,decision,weight_version,bias_version,status";
}

const char *decision_action_name(decision_action_t action)
{
    return action == DECISION_MEMORY ? "MOVE_MEMORY" : "MOVE_THREAD";
}

const char *decision_factor_name(decision_factor_t factor)
{
    if (factor < FACTOR_ACCESS || factor > FACTOR_SHARING)
        return "UNKNOWN";
    return factor_names[factor];
}

static double monotonic_seconds(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static long long epoch_seconds(void)
{
    return (long long)time(NULL);
}

static void timestamp_now(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm utc;

    gmtime_r(&now, &utc);
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static char *trim_field(char *field)
{
    char *end;

    while (isspace((unsigned char)*field))
        field++;
    end = field + strlen(field);
    while (end > field && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return field;
}

static size_t split_csv_line(char *line, char **fields)
{
    size_t count = 0;
    char *start = line;
    char *cursor = line;

    while (*cursor != '\0') {
        if (*cursor == ',') {
            *cursor = '\0';
            if (count < MAX_CSV_COLUMNS)
                fields[count++] = trim_field(start);
            start = cursor + 1;
        } else if (*cursor == '\n' || *cursor == '\r') {
            *cursor = '\0';
            if (count < MAX_CSV_COLUMNS)
                fields[count++] = trim_field(start);
            return count;
        }
        cursor++;
    }
    if (count < MAX_CSV_COLUMNS)
        fields[count++] = trim_field(start);
    return count;
}

static int header_column(char **fields, size_t count, const char *name)
{
    size_t index;

    for (index = 0; index < count; index++)
        if (strcmp(fields[index], name) == 0)
            return (int)index;
    return -1;
}

static char *duplicate_string(const char *value)
{
    size_t length = strlen(value) + 1;
    char *copy = malloc(length);

    if (copy != NULL)
        memcpy(copy, value, length);
    return copy;
}

static bool unavailable_value(const char *text)
{
    return text == NULL || *text == '\0' || strcmp(text, "NA") == 0 || strcmp(text, "-1") == 0 || strcmp(text, "-1.0") == 0;
}

static int parse_double(const char *text, double *value)
{
    char *end = NULL;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed))
        return -1;
    *value = parsed;
    return 0;
}

static int parse_long(const char *text, long *value)
{
    char *end = NULL;
    long parsed;

    if (text == NULL || *text == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0)
        return -1;
    *value = parsed;
    return 0;
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static bool valid_id(const char *value)
{
    return value != NULL && *value != '\0' && strlen(value) <= MAX_ID_LENGTH && strchr(value, ',') == NULL;
}

static int parse_action(const char *text, decision_action_t *action)
{
    if (strcasecmp(text, "MEMORY") == 0) {
        *action = DECISION_MEMORY;
        return 0;
    }
    if (strcasecmp(text, "THREAD") == 0) {
        *action = DECISION_THREAD;
        return 0;
    }
    return -1;
}

static int parse_factor(const char *text, decision_factor_t *factor)
{
    for (int index = 0; index < (int)DECISION_FACTOR_COUNT; index++) {
        if (strcasecmp(text, factor_names[index]) == 0) {
            *factor = (decision_factor_t)index;
            return 0;
        }
    }
    return -1;
}

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int path_join(char *buffer, size_t size, const char *directory, const char *name)
{
    int written = snprintf(buffer, size, "%s/%s", directory, name);

    return written < 0 || (size_t)written >= size ? -1 : 0;
}

static int write_weight_records_file(const char *path, weight_record_t *records, size_t count)
{
    FILE *file = NULL;
    char timestamp[32];
    size_t index;

    timestamp_now(timestamp, sizeof(timestamp));
    file = fopen(path, "w");
    if (file == NULL)
        return -1;
    fprintf(file, "app_id,action,factor,weight,version,updated_at\n");
    for (index = 0; index < count; index++)
        fprintf(file, "%s,%s,%s,%.12f,%u,%s\n", records[index].app_id,
                records[index].action == DECISION_MEMORY ? "MEMORY" : "THREAD",
                decision_factor_name(records[index].factor), records[index].weight,
                records[index].version, records[index].updated_at[0] ? records[index].updated_at : timestamp);
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int save_weight_records(const char *path, weight_record_t *records, size_t count)
{
    char temporary[512];

    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        return -1;
    if (write_weight_records_file(temporary, records, count) != 0) {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static void free_weight_records(weight_record_t *records, size_t count)
{
    for (size_t index = 0; index < count; index++)
        free(records[index].app_id);
    free(records);
}

static int load_weight_records(const char *path, weight_record_t **records, size_t *count)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    char *fields[MAX_CSV_COLUMNS];
    ssize_t length;

    *records = NULL;
    *count = 0;
    file = fopen(path, "r");
    if (file == NULL && errno == ENOENT)
        return 0;
    if (file == NULL)
        return -1;
    length = getline(&line, &capacity, file);
    if (length < 0 || split_csv_line(line, fields) < 6 || strcmp(fields[0], "app_id") != 0) {
        fclose(file);
        free(line);
        return -1;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        size_t field_count = split_csv_line(line, fields);
        weight_record_t record;
        char *copy;
        uint64_t version;

        (void)length;
        if (field_count < 6 || !valid_id(fields[0]) || parse_action(fields[1], &record.action) != 0 ||
            parse_factor(fields[2], &record.factor) != 0 || parse_double(fields[3], &record.weight) != 0 ||
            parse_u64(fields[4], &version) != 0 || version == 0 || record.weight < 0.0) {
            free_weight_records(*records, *count);
            fclose(file);
            free(line);
            return -1;
        }
        copy = duplicate_string(fields[0]);
        if (copy == NULL) {
            free_weight_records(*records, *count);
            fclose(file);
            free(line);
            return -1;
        }
        record.app_id = copy;
        record.version = (unsigned)version;
        snprintf(record.updated_at, sizeof(record.updated_at), "%s", fields[5]);
        weight_record_t *expanded = realloc(*records, (*count + 1) * sizeof(**records));
        if (expanded == NULL) {
            free(record.app_id);
            free_weight_records(*records, *count);
            fclose(file);
            free(line);
            return -1;
        }
        *records = expanded;
        (*records)[(*count)++] = record;
    }
    int read_error = ferror(file);
    fclose(file);
    free(line);
    return read_error ? -1 : 0;
}

static int find_weight_record(weight_record_t *records, size_t count, const char *app_id,
                              decision_action_t action, decision_factor_t factor)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(records[index].app_id, app_id) == 0 && records[index].action == action && records[index].factor == factor)
            return (int)index;
    return -1;
}

static int add_weight_record(weight_record_t **records, size_t *count, const char *app_id,
                             decision_action_t action, decision_factor_t factor,
                             double value, unsigned version)
{
    weight_record_t *expanded = realloc(*records, (*count + 1) * sizeof(**records));

    if (expanded == NULL)
        return -1;
    *records = expanded;
    (*records)[*count].app_id = duplicate_string(app_id);
    if ((*records)[*count].app_id == NULL)
        return -1;
    (*records)[*count].action = action;
    (*records)[*count].factor = factor;
    (*records)[*count].weight = value;
    (*records)[*count].version = version;
    (*records)[*count].updated_at[0] = '\0';
    (*count)++;
    return 0;
}

static double default_weight(decision_action_t action, decision_factor_t factor)
{
    return action == DECISION_MEMORY ? default_memory_weights[factor] : default_thread_weights[factor];
}

static void context_set_weights(application_context_t *context, weight_record_t *records, size_t count)
{
    for (int action = 0; action < 2; action++) {
        for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++) {
            int found = find_weight_record(records, count, context->id,
                                           (decision_action_t)action, (decision_factor_t)factor);
            double value = found >= 0 ? records[found].weight : default_weight((decision_action_t)action, (decision_factor_t)factor);

            if (action == DECISION_MEMORY)
                context->memory_weights[factor] = value;
            else
                context->thread_weights[factor] = value;
        }
    }
}

static int append_weight_history(const char *path, const char *app_id, decision_action_t action,
                                 decision_factor_t factor, double old_value, double new_value,
                                 unsigned version)
{
    FILE *file = fopen(path, "a+");
    char line[512];
    char timestamp[32];
    long long now = epoch_seconds();

    if (file == NULL)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    if (ftell(file) == 0)
        fprintf(file, "%s\n", HISTORY_WEIGHT_HEADER);
    timestamp_now(timestamp, sizeof(timestamp));
    snprintf(line, sizeof(line), "%s,%s,%s,%.12f,%.12f,%u,%s,1.000000000,%lld\n", app_id,
             action == DECISION_MEMORY ? "MEMORY" : "THREAD", decision_factor_name(factor),
             old_value, new_value, version, timestamp, now);
    fputs(line, file);
    fclose(file);
    return 0;
}

static int write_bias_records_file(const char *path, bias_record_t *records, size_t count)
{
    FILE *file;
    char timestamp[32];

    timestamp_now(timestamp, sizeof(timestamp));
    file = fopen(path, "w");
    if (file == NULL)
        return -1;
    fprintf(file, "app_id,memory_bias,thread_bias,version,updated_at\n");
    for (size_t index = 0; index < count; index++)
        fprintf(file, "%s,%.12f,%.12f,%u,%s\n", records[index].app_id,
                records[index].memory_bias, records[index].thread_bias, records[index].version,
                records[index].updated_at[0] ? records[index].updated_at : timestamp);
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int save_bias_records(const char *path, bias_record_t *records, size_t count)
{
    char temporary[512];

    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        return -1;
    if (write_bias_records_file(temporary, records, count) != 0) {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static void free_bias_records(bias_record_t *records, size_t count)
{
    for (size_t index = 0; index < count; index++)
        free(records[index].app_id);
    free(records);
}

static int load_bias_records(const char *path, bias_record_t **records, size_t *count)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    char *fields[MAX_CSV_COLUMNS];
    ssize_t length;

    *records = NULL;
    *count = 0;
    file = fopen(path, "r");
    if (file == NULL && errno == ENOENT)
        return 0;
    if (file == NULL)
        return -1;
    length = getline(&line, &capacity, file);
    if (length < 0 || split_csv_line(line, fields) < 5 || strcmp(fields[0], "app_id") != 0) {
        fclose(file);
        free(line);
        return -1;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        size_t field_count = split_csv_line(line, fields);
        bias_record_t record;
        uint64_t version;
        char *copy;
        (void)length;

        if (field_count < 5 || !valid_id(fields[0]) || parse_double(fields[1], &record.memory_bias) != 0 ||
            parse_double(fields[2], &record.thread_bias) != 0 || parse_u64(fields[3], &version) != 0 || version == 0) {
            free_bias_records(*records, *count);
            fclose(file);
            free(line);
            return -1;
        }
        copy = duplicate_string(fields[0]);
        if (copy == NULL) {
            free_bias_records(*records, *count);
            fclose(file);
            free(line);
            return -1;
        }
        record.app_id = copy;
        record.version = (unsigned)version;
        snprintf(record.updated_at, sizeof(record.updated_at), "%s", fields[4]);
        bias_record_t *expanded = realloc(*records, (*count + 1) * sizeof(**records));
        if (expanded == NULL) {
            free(record.app_id);
            free_bias_records(*records, *count);
            fclose(file);
            free(line);
            return -1;
        }
        *records = expanded;
        (*records)[(*count)++] = record;
    }
    fclose(file);
    free(line);
    return 0;
}

static int find_bias_record(bias_record_t *records, size_t count, const char *app_id)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(records[index].app_id, app_id) == 0)
            return (int)index;
    return -1;
}

static double clamp_bias(double value, double minimum, double maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static int add_bias_record(bias_record_t **records, size_t *count, const char *app_id,
                           double memory_bias, double thread_bias, unsigned version)
{
    bias_record_t *expanded = realloc(*records, (*count + 1) * sizeof(**records));

    if (expanded == NULL)
        return -1;
    *records = expanded;
    (*records)[*count].app_id = duplicate_string(app_id);
    if ((*records)[*count].app_id == NULL)
        return -1;
    (*records)[*count].memory_bias = memory_bias;
    (*records)[*count].thread_bias = thread_bias;
    (*records)[*count].version = version;
    (*records)[*count].updated_at[0] = '\0';
    (*count)++;
    return 0;
}

static int load_context(const decision_config_t *config, const char *app_id,
                        application_context_t *context, const char *weight_path,
                        const char *bias_path, const char *weight_history_path)
{
    weight_record_t *weights = NULL;
    bias_record_t *biases = NULL;
    size_t weight_count = 0;
    size_t bias_count = 0;
    bool weights_changed = false;
    bool biases_changed = false;
    int bias_index;
    char timestamp[32];

    memset(context, 0, sizeof(*context));
    context->id = duplicate_string(app_id);
    if (context->id == NULL || load_weight_records(weight_path, &weights, &weight_count) != 0 ||
        load_bias_records(bias_path, &biases, &bias_count) != 0)
        goto fail;
    timestamp_now(timestamp, sizeof(timestamp));
    for (int action = 0; action < 2; action++) {
        for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++) {
            if (find_weight_record(weights, weight_count, GLOBAL_APP_ID,
                                   (decision_action_t)action, (decision_factor_t)factor) < 0) {
                if (add_weight_record(&weights, &weight_count, GLOBAL_APP_ID,
                                      (decision_action_t)action, (decision_factor_t)factor,
                                      default_weight((decision_action_t)action, (decision_factor_t)factor), 1) != 0)
                    goto fail;
                weights_changed = true;
            }
        }
    }
    for (int action = 0; action < 2; action++) {
        for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++) {
            int app_weight = find_weight_record(weights, weight_count, app_id,
                                                (decision_action_t)action, (decision_factor_t)factor);
            int global_weight = find_weight_record(weights, weight_count, GLOBAL_APP_ID,
                                                   (decision_action_t)action, (decision_factor_t)factor);
            if (app_weight < 0) {
                if (add_weight_record(&weights, &weight_count, app_id, (decision_action_t)action,
                                      (decision_factor_t)factor, weights[global_weight].weight,
                                      weights[global_weight].version) != 0)
                    goto fail;
                weights_changed = true;
            }
        }
    }
    context_set_weights(context, weights, weight_count);
    context->weight_version = 1;
    for (size_t index = 0; index < weight_count; index++)
        if (strcmp(weights[index].app_id, app_id) == 0 && weights[index].version > context->weight_version)
            context->weight_version = weights[index].version;
    for (size_t override = 0; override < config->weight_override_count; override++) {
        decision_weight_override_t requested = config->weight_overrides[override];
        int found = find_weight_record(weights, weight_count, app_id, requested.action, requested.factor);

        if (found < 0)
            goto fail;
        if (weights[found].weight != requested.value) {
            double old_value = weights[found].weight;
            weights[found].weight = requested.value;
            weights[found].version++;
            context->weight_version = weights[found].version;
            if (requested.action == DECISION_MEMORY)
                context->memory_weights[requested.factor] = requested.value;
            else
                context->thread_weights[requested.factor] = requested.value;
            append_weight_history(weight_history_path, app_id, requested.action, requested.factor,
                                  old_value, requested.value, weights[found].version);
            weights_changed = true;
        }
    }
    if (weights_changed && save_weight_records(weight_path, weights, weight_count) != 0)
        goto fail;

    bias_index = find_bias_record(biases, bias_count, GLOBAL_APP_ID);
    if (bias_index < 0) {
        if (add_bias_record(&biases, &bias_count, GLOBAL_APP_ID, 0.0, 0.0, 1) != 0)
            goto fail;
        bias_index = (int)(bias_count - 1);
        biases_changed = true;
    }
    int app_bias = find_bias_record(biases, bias_count, app_id);
    if (app_bias < 0) {
        if (add_bias_record(&biases, &bias_count, app_id, biases[bias_index].memory_bias,
                            biases[bias_index].thread_bias, biases[bias_index].version) != 0)
            goto fail;
        app_bias = (int)(bias_count - 1);
        biases_changed = true;
    }
    context->memory_bias = clamp_bias(biases[app_bias].memory_bias, config->bias_min, config->bias_max);
    context->thread_bias = clamp_bias(biases[app_bias].thread_bias, config->bias_min, config->bias_max);
    context->bias_version = biases[app_bias].version;
    if (context->memory_bias != biases[app_bias].memory_bias || context->thread_bias != biases[app_bias].thread_bias) {
        biases[app_bias].memory_bias = context->memory_bias;
        biases[app_bias].thread_bias = context->thread_bias;
        biases_changed = true;
    }
    if (config->override_bias && (context->memory_bias != config->default_memory_bias ||
                                  context->thread_bias != config->default_thread_bias)) {
        context->memory_bias = clamp_bias(config->default_memory_bias, config->bias_min, config->bias_max);
        context->thread_bias = clamp_bias(config->default_thread_bias, config->bias_min, config->bias_max);
        biases[app_bias].memory_bias = context->memory_bias;
        biases[app_bias].thread_bias = context->thread_bias;
        biases[app_bias].version++;
        context->bias_version = biases[app_bias].version;
        biases_changed = true;
    }
    if (biases_changed && save_bias_records(bias_path, biases, bias_count) != 0)
        goto fail;
    free_weight_records(weights, weight_count);
    free_bias_records(biases, bias_count);
    return 0;

fail:
    free(context->id);
    context->id = NULL;
    free_weight_records(weights, weight_count);
    free_bias_records(biases, bias_count);
    return -1;
}

static void free_context(application_context_t *context)
{
    for (size_t index = 0; index < context->entities_count; index++)
        free(context->entities[index]);
    free(context->entities);
    free(context->id);
}

static int find_context(application_context_t *contexts, size_t count, const char *app_id)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(contexts[index].id, app_id) == 0)
            return (int)index;
    return -1;
}

static int context_add_entity(application_context_t *context, const char *entity_id)
{
    for (size_t index = 0; index < context->entities_count; index++)
        if (strcmp(context->entities[index], entity_id) == 0)
            return 0;
    char **expanded = realloc(context->entities, (context->entities_count + 1) * sizeof(*expanded));
    if (expanded == NULL)
        return -1;
    context->entities = expanded;
    context->entities[context->entities_count] = duplicate_string(entity_id);
    if (context->entities[context->entities_count] == NULL)
        return -1;
    context->entities_count++;
    return 0;
}

static int decision_columns_from_header(char **fields, size_t count, decision_columns_t *columns)
{
    memset(columns, -1, sizeof(*columns));
    columns->count = count;
    columns->timestamp = header_column(fields, count, "timestamp");
    columns->elapsed_ms = header_column(fields, count, "elapsed_ms");
    columns->pid = header_column(fields, count, "pid");
    columns->app_id = header_column(fields, count, "app_id");
    columns->entity_id = header_column(fields, count, "entity_id");
    columns->classification = header_column(fields, count, "classification");
    if (columns->classification < 0)
        columns->classification = header_column(fields, count, "current_class");
    columns->score = header_column(fields, count, "classification_score");
    if (columns->score < 0)
        columns->score = header_column(fields, count, "score");
    columns->factors[0] = header_column(fields, count, "f_access");
    columns->factors[1] = header_column(fields, count, "f_threshold");
    columns->factors[2] = header_column(fields, count, "f_gain_memory");
    columns->factors[3] = header_column(fields, count, "f_cost_memory");
    columns->factors[4] = header_column(fields, count, "f_cpu_memory");
    columns->factors[5] = header_column(fields, count, "f_sharing_memory");
    columns->factors[6] = header_column(fields, count, "f_gain_thread");
    columns->factors[7] = header_column(fields, count, "f_cost_thread");
    columns->factors[8] = header_column(fields, count, "f_cpu_thread");
    columns->factors[9] = header_column(fields, count, "f_sharing_thread");
    return columns->timestamp >= 0 && columns->pid >= 0 && columns->classification >= 0 && columns->score >= 0 ? 0 : -1;
}

static int parse_classification(const char *text, bool *available)
{
    if (strcasecmp(text, "HOT") == 0)
        return *available = true, 2;
    if (strcasecmp(text, "MODERATE") == 0)
        return *available = true, 1;
    if (strcasecmp(text, "COLD") == 0)
        return *available = true, 0;
    *available = false;
    return -1;
}

static int parse_factors(char **fields, size_t count, const decision_columns_t *columns,
                         factor_values_t *factors)
{
    const char *names[] = {"f_access", "f_threshold", "f_gain_memory", "f_cost_memory", "f_cpu_memory", "f_sharing_memory", "f_gain_thread", "f_cost_thread", "f_cpu_thread", "f_sharing_thread"};
    factors->missing[0] = '\0';
    for (int index = 0; index < 10; index++) {
        int column = columns->factors[index];

        factors->values[index] = -1.0;
        factors->available[index] = false;
        if (column < 0 || (size_t)column >= count || unavailable_value(fields[column])) {
            size_t used = strlen(factors->missing);
            snprintf(factors->missing + used, sizeof(factors->missing) - used, "%s%s",
                     used == 0 ? "" : ";", names[index]);
            continue;
        }
        if (parse_double(fields[column], &factors->values[index]) != 0 ||
            factors->values[index] < 0.0 || factors->values[index] > 1.0)
            return -1;
        factors->available[index] = true;
    }
    return 0;
}

static bool all_factors_available(const factor_values_t *factors)
{
    for (int index = 0; index < 10; index++)
        if (!factors->available[index])
            return false;
    return true;
}

static decision_result_t calculate_decision(const application_context_t *context,
                                            const factor_values_t *factors,
                                            double epsilon)
{
    decision_result_t result;
    double memory = context->memory_weights[FACTOR_ACCESS] * factors->values[0] +
                    context->memory_weights[FACTOR_THRESHOLD] * factors->values[1] +
                    context->memory_weights[FACTOR_GAIN] * factors->values[2] -
                    context->memory_weights[FACTOR_COST] * factors->values[3] -
                    context->memory_weights[FACTOR_CPU] * factors->values[4] -
                    context->memory_weights[FACTOR_SHARING] * factors->values[5];
    double thread = context->thread_weights[FACTOR_ACCESS] * factors->values[0] +
                    context->thread_weights[FACTOR_THRESHOLD] * factors->values[1] +
                    context->thread_weights[FACTOR_GAIN] * factors->values[6] -
                    context->thread_weights[FACTOR_COST] * factors->values[7] -
                    context->thread_weights[FACTOR_CPU] * factors->values[8] -
                    context->thread_weights[FACTOR_SHARING] * factors->values[9];

    result.raw_memory = memory;
    result.raw_thread = thread;
    result.final_memory = memory + context->memory_bias;
    result.final_thread = thread + context->thread_bias;
    if (result.final_memory <= 0.0 && result.final_thread <= 0.0)
        result.decision = "NO_MIGRATION";
    else if (result.final_memory > result.final_thread + epsilon)
        result.decision = "MOVE_MEMORY";
    else if (result.final_thread > result.final_memory + epsilon)
        result.decision = "MOVE_THREAD";
    else
        result.decision = "NO_MIGRATION";
    return result;
}

static int append_decision_history(const char *path, const char *line)
{
    FILE *file = fopen(path, "a+");
    long long now = epoch_seconds();

    if (file == NULL)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    if (ftell(file) == 0)
        fprintf(file, "%s\n", HISTORY_DECISION_HEADER);
    fprintf(file, "%s,1.000000000,%lld\n", line, now);
    fclose(file);
    return 0;
}

static int compact_history(const char *path, size_t max_records, double max_days,
                           double decay_lambda)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    char *header = NULL;
    char **records = NULL;
    size_t count = 0;
    long long now = epoch_seconds();
    long long max_age = (long long)(max_days * 86400.0);
    char temporary[512];
    FILE *output;

    file = fopen(path, "r");
    if (file == NULL && errno == ENOENT)
        return 0;
    if (file == NULL)
        return -1;
    length = getline(&line, &capacity, file);
    if (length < 0) {
        fclose(file);
        free(line);
        return -1;
    }
    header = duplicate_string(line);
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *last_comma;
        long long recorded;
        char *copy;
        char **expanded;

        last_comma = strrchr(line, ',');
        recorded = last_comma != NULL ? strtoll(last_comma + 1, NULL, 10) : now;
        if (recorded > 0 && now - recorded > max_age)
            continue;
        copy = duplicate_string(line);
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
    fclose(file);
    file = NULL;
    if (count > max_records) {
        size_t first = count - max_records;
        for (size_t index = 0; index < first; index++)
            free(records[index]);
        memmove(records, records + first, max_records * sizeof(*records));
        count = max_records;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
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
        fprintf(output, "%.9f%s", exp(-decay_lambda * age_days), last_comma);
    }
    if (fflush(output) != 0 || fsync(fileno(output)) != 0 || fclose(output) != 0) {
        unlink(temporary);
        goto fail;
    }
    if (rename(temporary, path) != 0)
        goto fail;
    for (size_t index = 0; index < count; index++)
        free(records[index]);
    free(records);
    free(header);
    free(line);
    return 0;

fail:
    if (file != NULL)
        fclose(file);
    for (size_t index = 0; index < count; index++)
        free(records[index]);
    free(records);
    free(header);
    free(line);
    return -1;
}

static void free_application_records(application_record_t *records, size_t count)
{
    for (size_t index = 0; index < count; index++)
        free(records[index].app_id);
    free(records);
}

static long long parse_timestamp_epoch(const char *text)
{
    struct tm value;

    memset(&value, 0, sizeof(value));
    if (strptime(text, "%Y-%m-%dT%H:%M:%SZ", &value) == NULL)
        return -1;
    return (long long)timegm(&value);
}

static int update_application_state(const char *path, application_context_t *contexts, size_t context_count,
                                    double inactive_days, double expire_days)
{
    application_record_t *records = NULL;
    size_t record_count = 0;
    FILE *file = NULL;
    char *line = NULL;
    size_t capacity = 0;
    char *fields[MAX_CSV_COLUMNS];
    ssize_t length;
    char temporary[512];
    char timestamp[32];

    file = fopen(path, "r");
    if (file != NULL) {
        length = getline(&line, &capacity, file);
        if (length < 0 || split_csv_line(line, fields) < 8 || strcmp(fields[0], "app_id") != 0) {
            fclose(file);
            free(line);
            return -1;
        }
        while ((length = getline(&line, &capacity, file)) >= 0) {
            size_t count = split_csv_line(line, fields);
            application_record_t record;
            uint64_t entity_count;
            uint64_t hot_count;
            uint64_t moderate_count;
            uint64_t cold_count;
            uint64_t profile_version;
            (void)length;

            if (count < 8 || !valid_id(fields[0]) || parse_long(fields[1], &record.pid) != 0 ||
                parse_u64(fields[2], &entity_count) != 0 || parse_u64(fields[3], &hot_count) != 0 ||
                parse_u64(fields[4], &moderate_count) != 0 || parse_u64(fields[5], &cold_count) != 0 ||
                parse_u64(fields[6], &profile_version) != 0) {
                free_application_records(records, record_count);
                fclose(file);
                free(line);
                return -1;
            }
            record.app_id = duplicate_string(fields[0]);
            if (record.app_id == NULL) {
                free_application_records(records, record_count);
                fclose(file);
                free(line);
                return -1;
            }
            record.entity_count = (size_t)entity_count;
            record.hot_count = (size_t)hot_count;
            record.moderate_count = (size_t)moderate_count;
            record.cold_count = (size_t)cold_count;
            record.profile_version = (unsigned)profile_version;
            if (record.profile_version == 0) {
                free(record.app_id);
                free_application_records(records, record_count);
                fclose(file);
                free(line);
                return -1;
            }
            snprintf(record.last_seen, sizeof(record.last_seen), "%s", fields[7]);
            snprintf(record.status, sizeof(record.status), "%s", count > 8 ? fields[8] : "ACTIVE");
            application_record_t *expanded = realloc(records, (record_count + 1) * sizeof(*records));
            if (expanded == NULL) {
                free(record.app_id);
                free_application_records(records, record_count);
                fclose(file);
                free(line);
                return -1;
            }
            records = expanded;
            records[record_count++] = record;
        }
        fclose(file);
        file = NULL;
    } else if (errno != ENOENT) {
        free(line);
        return -1;
    }
    for (size_t index = 0; index < record_count; index++) {
        long long last_seen = parse_timestamp_epoch(records[index].last_seen);
        double age_days = last_seen >= 0 && epoch_seconds() >= last_seen
                              ? (double)(epoch_seconds() - last_seen) / 86400.0 : 0.0;

        if (age_days >= expire_days)
            snprintf(records[index].status, sizeof(records[index].status), "EXPIRED");
        else if (age_days >= inactive_days)
            snprintf(records[index].status, sizeof(records[index].status), "INACTIVE");
    }
    timestamp_now(timestamp, sizeof(timestamp));
    for (size_t context_index = 0; context_index < context_count; context_index++) {
        application_context_t *context = &contexts[context_index];
        size_t record_index = 0;
        while (record_index < record_count && strcmp(records[record_index].app_id, context->id) != 0)
            record_index++;
        if (record_index == record_count) {
            application_record_t *expanded = realloc(records, (record_count + 1) * sizeof(*records));
            if (expanded == NULL)
                goto fail;
            records = expanded;
            records[record_count].app_id = duplicate_string(context->id);
            if (records[record_count].app_id == NULL)
                goto fail;
            record_index = record_count++;
            records[record_index].profile_version = 1;
        }
        records[record_index].pid = context->last_pid;
        records[record_index].entity_count = context->entities_count;
        records[record_index].hot_count = context->hot_count;
        records[record_index].moderate_count = context->moderate_count;
        records[record_index].cold_count = context->cold_count;
        snprintf(records[record_index].last_seen, sizeof(records[record_index].last_seen), "%s",
                 context->last_seen[0] ? context->last_seen : timestamp);
        snprintf(records[record_index].status, sizeof(records[record_index].status), "ACTIVE");
    }
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        goto fail;
    file = fopen(temporary, "w");
    if (file == NULL)
        goto fail;
    fprintf(file, "app_id,pid,entity_count,hot_count,moderate_count,cold_count,current_profile_version,last_seen,status\n");
    for (size_t index = 0; index < record_count; index++)
        fprintf(file, "%s,%ld,%zu,%zu,%zu,%zu,%u,%s,%s\n", records[index].app_id,
                records[index].pid, records[index].entity_count, records[index].hot_count,
                records[index].moderate_count, records[index].cold_count, records[index].profile_version,
                records[index].last_seen, records[index].status);
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        unlink(temporary);
        fclose(file);
        file = NULL;
        goto fail;
    }
    if (fclose(file) != 0) {
        file = NULL;
        unlink(temporary);
        goto fail;
    }
    file = NULL;
    if (rename(temporary, path) != 0)
        goto fail;
    free_application_records(records, record_count);
    free(line);
    return 0;

fail:
    if (file != NULL)
        fclose(file);
    unlink(temporary);
    free_application_records(records, record_count);
    free(line);
    return -1;
}

static int append_log(const char *path, const char *message)
{
    FILE *file = fopen(path, "a");

    if (file == NULL)
        return -1;
    fprintf(file, "%lld %s\n", epoch_seconds(), message);
    fclose(file);
    return 0;
}

static int write_decision_line(FILE *output, const char *history_path,
                               const char *timestamp, const char *app_id, long pid,
                               const char *entity_id, const char *classification,
                               double classification_score, const factor_values_t *factors,
                               const application_context_t *context, const decision_result_t *result,
                               const decision_config_t *config, const char *status)
{
    char line[4096];
    int written;

    written = snprintf(line, sizeof(line),
        "%s,%s,%ld,%s,%s,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%s,%u,%u,%s",
        timestamp, app_id, pid, entity_id, classification, classification_score,
        factors->values[0], factors->values[1], factors->values[2], factors->values[3],
        factors->values[4], factors->values[5], factors->values[6], factors->values[7],
        factors->values[8], factors->values[9], result->raw_memory, result->raw_thread,
        context->memory_bias, context->thread_bias, result->final_memory, result->final_thread,
        config->epsilon, result->decision, context->weight_version, context->bias_version, status);
    if (written < 0 || (size_t)written >= sizeof(line))
        return -1;
    fprintf(output, "%s\n", line);
    fflush(output);
    return append_decision_history(history_path, line);
}

static int validate_config(const decision_config_t *config)
{
    if (config == NULL || config->input_path == NULL || config->output_path == NULL ||
        config->state_dir == NULL || config->history_dir == NULL || config->log_path == NULL ||
        config->epsilon < 0.0 || !isfinite(config->epsilon) ||
        config->bias_min > config->bias_max || !isfinite(config->bias_min) || !isfinite(config->bias_max) ||
        config->history_max_records == 0 || config->history_max_days <= 0.0 ||
        config->history_decay_lambda <= 0.0 || config->cleanup_interval == 0)
        return -1;
    return 0;
}

int decision_run(const decision_config_t *config, decision_summary_t *summary)
{
    FILE *input = NULL;
    FILE *output = NULL;
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t length;
    char *fields[MAX_CSV_COLUMNS];
    decision_columns_t columns;
    application_context_t *contexts = NULL;
    size_t context_count = 0;
    uint64_t previous_elapsed = 0;
    bool have_elapsed = false;
    char weight_path[512], bias_path[512], application_path[512];
    char history_path[512], weight_history_path[512];
    double start_time;
    int result = -1;
    monitor_profile_scope_t prepare_profile;
    monitor_profile_scope_t input_profile;

    if (summary != NULL)
        memset(summary, 0, sizeof(*summary));
    if (validate_config(config) != 0) {
        fprintf(stderr, "Error: invalid decision engine configuration.\n");
        return -1;
    }
    monitor_profile_scope_begin(&prepare_profile, "phase56_child", "p5_state_path_prepare", config->app_id, -1, 0);
    if (ensure_directory(config->state_dir) != 0 || ensure_directory(config->history_dir) != 0 ||
        path_join(weight_path, sizeof(weight_path), config->state_dir, "weights.csv") != 0 ||
        path_join(bias_path, sizeof(bias_path), config->state_dir, "biases.csv") != 0 ||
        path_join(application_path, sizeof(application_path), config->state_dir, "application_state.csv") != 0 ||
        path_join(history_path, sizeof(history_path), config->history_dir, "decisions.csv") != 0 ||
        path_join(weight_history_path, sizeof(weight_history_path), config->history_dir, "weight_history.csv") != 0) {
        fprintf(stderr, "Error: cannot prepare decision state/history paths.\n");
        monitor_profile_scope_end(&prepare_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&prepare_profile, "OK");
    monitor_profile_scope_begin(&input_profile, "phase56_child", "p5_input_open_parse", config->app_id, -1, 0);
    input = fopen(config->input_path, "r");
    if (input == NULL) {
        fprintf(stderr, "Error: cannot open decision input '%s': %s\n", config->input_path, strerror(errno));
        monitor_profile_scope_end(&input_profile, "ERROR");
        return -1;
    }
    length = getline(&line, &line_capacity, input);
    if (length < 0) {
        fprintf(stderr, "Error: decision input is empty.\n");
        monitor_profile_scope_end(&input_profile, "ERROR");
        goto cleanup;
    }
    if (decision_columns_from_header((fields), split_csv_line(line, fields), &columns) != 0) {
        fprintf(stderr, "Error: decision input requires timestamp, pid, classification/current_class, and score/classification_score columns.\n");
        monitor_profile_scope_end(&input_profile, "ERROR");
        goto cleanup;
    }
    monitor_profile_scope_end(&input_profile, "OK");
    output = fopen(config->output_path, "w");
    if (output == NULL) {
        fprintf(stderr, "Error: cannot open decision output '%s': %s\n", config->output_path, strerror(errno));
        goto cleanup;
    }
    fprintf(output, "%s\n", output_header());
    start_time = monotonic_seconds();
    while ((length = getline(&line, &line_capacity, input)) >= 0) {
        size_t field_count = split_csv_line(line, fields);
        uint64_t elapsed_ms = 0;
        long pid;
        double classification_score;
        bool class_available;
        const char *app_id;
        const char *entity_id = DEFAULT_ENTITY_ID;
        const char *classification_text;
        int context_index;
        application_context_t *context;
        factor_values_t factors;
        decision_result_t decision = {0};
        char timestamp[128];
        const char *status;
        (void)length;

        if ((size_t)columns.timestamp >= field_count || (size_t)columns.pid >= field_count ||
            (size_t)columns.classification >= field_count || (size_t)columns.score >= field_count) {
            fprintf(stderr, "Error: malformed decision input row %zu.\n", summary != NULL ? summary->input_rows + 2 : 0);
            goto cleanup;
        }
        if (fields[columns.timestamp][0] == '\0' || parse_long(fields[columns.pid], &pid) != 0 ||
            parse_double(fields[columns.score], &classification_score) != 0) {
            fprintf(stderr, "Error: invalid decision input identity/score row %zu.\n", summary != NULL ? summary->input_rows + 2 : 0);
            goto cleanup;
        }
        if (columns.elapsed_ms >= 0 && (size_t)columns.elapsed_ms < field_count &&
            !unavailable_value(fields[columns.elapsed_ms])) {
            if (parse_u64(fields[columns.elapsed_ms], &elapsed_ms) != 0)
                goto cleanup;
            if (have_elapsed && elapsed_ms < previous_elapsed) {
                fprintf(stderr, "Error: decision elapsed_ms is not ordered.\n");
                goto cleanup;
            }
            previous_elapsed = elapsed_ms;
            have_elapsed = true;
        }
        classification_text = fields[columns.classification];
        (void)parse_classification(classification_text, &class_available);
        app_id = columns.app_id >= 0 && (size_t)columns.app_id < field_count &&
                         valid_id(fields[columns.app_id]) && !unavailable_value(fields[columns.app_id])
                     ? fields[columns.app_id] : config->app_id;
        if (app_id == NULL || !valid_id(app_id)) {
            fprintf(stderr, "Error: application ID is required; supply --app-id or an app_id column.\n");
            goto cleanup;
        }
        if (columns.entity_id >= 0 && (size_t)columns.entity_id < field_count && valid_id(fields[columns.entity_id]))
            entity_id = fields[columns.entity_id];
        if (parse_factors(fields, field_count, &columns, &factors) != 0) {
            fprintf(stderr, "Error: decision factors must be finite normalized values in [0,1].\n");
            goto cleanup;
        }
        context_index = find_context(contexts, context_count, app_id);
        if (context_index < 0) {
            application_context_t *expanded = realloc(contexts, (context_count + 1) * sizeof(*contexts));
            monitor_profile_scope_t state_load_profile;
            if (expanded == NULL)
                goto cleanup;
            contexts = expanded;
            monitor_profile_scope_begin(&state_load_profile, "phase56_child", "p5_state_load_or_initialize",
                                        app_id, pid, 0);
            if (load_context(config, app_id, &contexts[context_count], weight_path, bias_path,
                              weight_history_path) != 0) {
                fprintf(stderr, "Error: cannot load state for application '%s'.\n", app_id);
                monitor_profile_scope_end(&state_load_profile, "ERROR");
                goto cleanup;
            }
            monitor_profile_scope_end(&state_load_profile, "OK");
            context_index = (int)context_count++;
        }
        context = &contexts[context_index];
        context->last_pid = pid;
        timestamp_now(context->last_seen, sizeof(context->last_seen));
        if (context_add_entity(context, entity_id) != 0)
            goto cleanup;
        if (class_available) {
            if (strcasecmp(classification_text, "HOT") == 0)
                context->hot_count++;
            else if (strcasecmp(classification_text, "MODERATE") == 0)
                context->moderate_count++;
            else if (strcasecmp(classification_text, "COLD") == 0)
                context->cold_count++;
        }
        if (class_available && classification_score >= 0.0 && all_factors_available(&factors)) {
            monitor_profile_scope_t compute_profile;

            monitor_profile_scope_begin(&compute_profile, "phase56_child", "p5_decision_compute", app_id, pid, 0);
            decision = calculate_decision(context, &factors, config->epsilon);
            monitor_profile_scope_end(&compute_profile, "OK");
            status = "DECISION_VALID";
            if (summary != NULL) {
                summary->valid_decisions++;
                if (strcmp(decision.decision, "MOVE_MEMORY") == 0)
                    summary->move_memory++;
                else if (strcmp(decision.decision, "MOVE_THREAD") == 0)
                    summary->move_thread++;
                else
                    summary->no_migration++;
            }
        } else {
            decision.raw_memory = -1.0;
            decision.raw_thread = -1.0;
            decision.final_memory = -1.0;
            decision.final_thread = -1.0;
            decision.decision = "INSUFFICIENT_DECISION_SIGNAL";
            status = factors.missing[0] ? factors.missing : "CLASSIFICATION_UNAVAILABLE";
            if (summary != NULL)
                summary->insufficient_rows++;
        }
        snprintf(timestamp, sizeof(timestamp), "%s", fields[columns.timestamp]);
        monitor_profile_scope_t output_profile;

        monitor_profile_scope_begin(&output_profile, "phase56_child", "p5_output_history_write", app_id, pid, 0);
        if (write_decision_line(output, history_path, timestamp, app_id, pid, entity_id,
                                classification_text, classification_score, &factors, context,
                                &decision, config, status) != 0) {
            monitor_profile_scope_end(&output_profile, "ERROR");
            goto cleanup;
        }
        monitor_profile_scope_end(&output_profile, "OK");
        if (summary != NULL)
            summary->input_rows++;
        if (summary != NULL && config->cleanup_interval > 0 && summary->input_rows % config->cleanup_interval == 0) {
            monitor_profile_scope_t cleanup_profile;

            monitor_profile_scope_begin(&cleanup_profile, "phase56_child", "p5_history_cleanup", app_id, pid, 0);
            compact_history(history_path, config->history_max_records, config->history_max_days,
                             config->history_decay_lambda);
            compact_history(weight_history_path, config->history_max_records, config->history_max_days,
                             config->history_decay_lambda);
            monitor_profile_scope_end(&cleanup_profile, "OK");
        }
    }
    if (ferror(input))
        goto cleanup;
    if (summary == NULL || summary->input_rows == 0) {
        fprintf(stderr, "Error: decision input has no data rows.\n");
        goto cleanup;
    }
    monitor_profile_scope_t persist_profile;
    monitor_profile_scope_begin(&persist_profile, "phase56_child", "p5_application_state_persist", config->app_id, -1, 0);
    for (size_t index = 0; index < context_count; index++) {
        if (update_application_state(application_path, &contexts[index], 1,
                                     config->inactive_days, config->expire_days) != 0) {
            monitor_profile_scope_end(&persist_profile, "ERROR");
            goto cleanup;
        }
    }
    monitor_profile_scope_end(&persist_profile, "OK");
    monitor_profile_scope_t final_cleanup_profile;
    monitor_profile_scope_begin(&final_cleanup_profile, "phase56_child", "p5_history_cleanup", config->app_id, -1, 0);
    compact_history(history_path, config->history_max_records, config->history_max_days,
                     config->history_decay_lambda);
    compact_history(weight_history_path, config->history_max_records, config->history_max_days,
                     config->history_decay_lambda);
    monitor_profile_scope_end(&final_cleanup_profile, "OK");
    monitor_profile_scope_t log_profile;
    monitor_profile_scope_begin(&log_profile, "phase56_child", "p5_logging", config->app_id, -1, 0);
    append_log(config->log_path, "decision run completed; no migration executed");
    monitor_profile_scope_end(&log_profile, "OK");
    if (summary != NULL) {
        summary->applications = context_count;
        summary->runtime_seconds = monotonic_seconds() - start_time;
    }
    result = 0;

cleanup:
    for (size_t index = 0; index < context_count; index++)
        free_context(&contexts[index]);
    free(contexts);
    free(line);
    if (input != NULL)
        fclose(input);
    if (output != NULL)
        fclose(output);
    return result;
}

static double clamp_feedback_weight(double value, double minimum, double maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static int feedback_application_allowed(const char *path, const char *app_id)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    char *fields[MAX_CSV_COLUMNS];
    ssize_t length;
    int result = 0;

    file = fopen(path, "r");
    if (file == NULL)
        return errno == ENOENT ? 0 : -1;
    length = getline(&line, &capacity, file);
    if (length < 0 || split_csv_line(line, fields) < 8 || strcmp(fields[0], "app_id") != 0) {
        fclose(file);
        free(line);
        return -1;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        size_t count = split_csv_line(line, fields);

        if (count < 8 || !valid_id(fields[0])) {
            result = -1;
            break;
        }
        if (strcmp(fields[0], app_id) == 0 && count > 8 && strcmp(fields[8], "EXPIRED") == 0) {
            result = -2;
            break;
        }
    }
    if (ferror(file))
        result = -1;
    fclose(file);
    free(line);
    return result;
}

int decision_feedback_apply(const decision_config_t *config, const char *app_id,
                            const decision_feedback_update_t *update,
                            double min_weight, double max_weight,
                            decision_feedback_snapshot_t *snapshot)
{
    char weight_path[512];
    char bias_path[512];
    char weight_history_path[512];
    char application_path[512];
    char weight_temporary[512] = {0};
    char bias_temporary[512] = {0};
    char weight_backup_path[512] = {0};
    char bias_backup_path[512] = {0};
    weight_record_t *weights = NULL;
    bias_record_t *biases = NULL;
    size_t weight_count = 0;
    size_t bias_count = 0;
    bool weights_changed = false;
    bool biases_changed = false;
    bool weight_backup = false;
    bool bias_backup = false;
    bool weight_installed = false;
    bool bias_installed = false;
    int global_bias;
    int app_bias;

    if (snapshot != NULL)
        memset(snapshot, 0, sizeof(*snapshot));
    if (config == NULL || app_id == NULL || !valid_id(app_id) || update == NULL ||
        !isfinite(min_weight) || !isfinite(max_weight) || min_weight < 0.0 ||
        max_weight < min_weight || config->state_dir == NULL || config->history_dir == NULL ||
        path_join(weight_path, sizeof(weight_path), config->state_dir, "weights.csv") != 0 ||
        path_join(bias_path, sizeof(bias_path), config->state_dir, "biases.csv") != 0 ||
        path_join(weight_history_path, sizeof(weight_history_path), config->history_dir, "weight_history.csv") != 0 ||
        path_join(application_path, sizeof(application_path), config->state_dir, "application_state.csv") != 0)
        return -1;
    {
        int application_status = feedback_application_allowed(application_path, app_id);

        if (application_status != 0)
            return application_status == -2 ? -2 : -1;
    }
    for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++)
        if (!isfinite(update->memory_delta[factor]) || !isfinite(update->thread_delta[factor]))
            return -1;
    if (!isfinite(update->memory_bias_delta) || !isfinite(update->thread_bias_delta))
        return -1;
    if (ensure_directory(config->state_dir) != 0 || ensure_directory(config->history_dir) != 0 ||
        load_weight_records(weight_path, &weights, &weight_count) != 0 ||
        load_bias_records(bias_path, &biases, &bias_count) != 0)
        goto fail;
    for (int action = 0; action < 2; action++) {
        for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++) {
            int global = find_weight_record(weights, weight_count, GLOBAL_APP_ID,
                                            (decision_action_t)action, (decision_factor_t)factor);
            int app = find_weight_record(weights, weight_count, app_id,
                                         (decision_action_t)action, (decision_factor_t)factor);

            if (global < 0) {
                if (add_weight_record(&weights, &weight_count, GLOBAL_APP_ID,
                                      (decision_action_t)action, (decision_factor_t)factor,
                                      default_weight((decision_action_t)action, (decision_factor_t)factor), 1) != 0)
                    goto fail;
                global = (int)(weight_count - 1);
                weights_changed = true;
            }
            if (app < 0) {
                if (add_weight_record(&weights, &weight_count, app_id,
                                      (decision_action_t)action, (decision_factor_t)factor,
                                      weights[global].weight, weights[global].version) != 0)
                    goto fail;
                app = (int)(weight_count - 1);
                weights_changed = true;
            }
            if (weights[app].weight < min_weight || weights[app].weight > max_weight ||
                !isfinite(weights[app].weight))
                goto fail;
            if (snapshot != NULL) {
                if (action == DECISION_MEMORY) {
                    snapshot->old_memory_weights[factor] = weights[app].weight;
                    snapshot->new_memory_weights[factor] = clamp_feedback_weight(
                        weights[app].weight + update->memory_delta[factor], min_weight, max_weight);
                } else {
                    snapshot->old_thread_weights[factor] = weights[app].weight;
                    snapshot->new_thread_weights[factor] = clamp_feedback_weight(
                        weights[app].weight + update->thread_delta[factor], min_weight, max_weight);
                }
            }
        }
    }
    for (int action = 0; action < 2; action++) {
        for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++) {
            int app = find_weight_record(weights, weight_count, app_id,
                                         (decision_action_t)action, (decision_factor_t)factor);
            double delta = action == DECISION_MEMORY ? update->memory_delta[factor] : update->thread_delta[factor];
            double value = clamp_feedback_weight(weights[app].weight + delta, min_weight, max_weight);

            if (value != weights[app].weight) {
                weights[app].weight = value;
                weights[app].version++;
                weights[app].updated_at[0] = '\0';
                weights_changed = true;
            }
        }
    }
    global_bias = find_bias_record(biases, bias_count, GLOBAL_APP_ID);
    if (global_bias < 0) {
        if (add_bias_record(&biases, &bias_count, GLOBAL_APP_ID, 0.0, 0.0, 1) != 0)
            goto fail;
        global_bias = (int)(bias_count - 1);
        biases_changed = true;
    }
    app_bias = find_bias_record(biases, bias_count, app_id);
    if (app_bias < 0) {
        if (add_bias_record(&biases, &bias_count, app_id, biases[global_bias].memory_bias,
                            biases[global_bias].thread_bias, biases[global_bias].version) != 0)
            goto fail;
        app_bias = (int)(bias_count - 1);
        biases_changed = true;
    }
    if (!isfinite(biases[app_bias].memory_bias) || !isfinite(biases[app_bias].thread_bias) ||
        biases[app_bias].memory_bias < config->bias_min || biases[app_bias].memory_bias > config->bias_max ||
        biases[app_bias].thread_bias < config->bias_min || biases[app_bias].thread_bias > config->bias_max)
        goto fail;
    if (snapshot != NULL) {
        snapshot->old_memory_bias = biases[app_bias].memory_bias;
        snapshot->old_thread_bias = biases[app_bias].thread_bias;
        snapshot->new_memory_bias = clamp_bias(biases[app_bias].memory_bias + update->memory_bias_delta,
                                               config->bias_min, config->bias_max);
        snapshot->new_thread_bias = clamp_bias(biases[app_bias].thread_bias + update->thread_bias_delta,
                                               config->bias_min, config->bias_max);
    }
    {
        double memory_bias = clamp_bias(biases[app_bias].memory_bias + update->memory_bias_delta,
                                         config->bias_min, config->bias_max);
        double thread_bias = clamp_bias(biases[app_bias].thread_bias + update->thread_bias_delta,
                                         config->bias_min, config->bias_max);

        if (memory_bias != biases[app_bias].memory_bias || thread_bias != biases[app_bias].thread_bias) {
            biases[app_bias].memory_bias = memory_bias;
            biases[app_bias].thread_bias = thread_bias;
            biases[app_bias].version++;
            biases[app_bias].updated_at[0] = '\0';
            biases_changed = true;
        }
    }
    if ((weights_changed && (snprintf(weight_temporary, sizeof(weight_temporary), "%s.feedback.tmp", weight_path) >= (int)sizeof(weight_temporary) ||
                             snprintf(weight_backup_path, sizeof(weight_backup_path), "%s.feedback.bak", weight_path) >= (int)sizeof(weight_backup_path))) ||
        (biases_changed && (snprintf(bias_temporary, sizeof(bias_temporary), "%s.feedback.tmp", bias_path) >= (int)sizeof(bias_temporary) ||
                            snprintf(bias_backup_path, sizeof(bias_backup_path), "%s.feedback.bak", bias_path) >= (int)sizeof(bias_backup_path))))
        goto fail;
    if (weights_changed && write_weight_records_file(weight_temporary, weights, weight_count) != 0)
        goto fail;
    if (biases_changed && write_bias_records_file(bias_temporary, biases, bias_count) != 0)
        goto fail;
    if (weights_changed) {
        unlink(weight_backup_path);
        if (access(weight_path, F_OK) == 0) {
            if (rename(weight_path, weight_backup_path) != 0)
                goto fail;
            weight_backup = true;
        }
        if (rename(weight_temporary, weight_path) != 0)
            goto fail;
        weight_installed = true;
    }
    if (biases_changed) {
        unlink(bias_backup_path);
        if (access(bias_path, F_OK) == 0) {
            if (rename(bias_path, bias_backup_path) != 0)
                goto fail;
            bias_backup = true;
        }
        if (rename(bias_temporary, bias_path) != 0)
            goto fail;
        bias_installed = true;
    }
    if (weight_backup)
        unlink(weight_backup_path);
    if (bias_backup)
        unlink(bias_backup_path);
    if (snapshot != NULL) {
        snapshot->weight_version = 1;
        snapshot->bias_version = biases[app_bias].version;
        for (size_t index = 0; index < weight_count; index++)
            if (strcmp(weights[index].app_id, app_id) == 0 && weights[index].version > snapshot->weight_version)
                snapshot->weight_version = weights[index].version;
    }
    free_weight_records(weights, weight_count);
    free_bias_records(biases, bias_count);
    return 0;

fail:
    if (weight_installed)
        unlink(weight_path);
    if (bias_installed)
        unlink(bias_path);
    if (weight_backup)
        rename(weight_backup_path, weight_path);
    if (bias_backup)
        rename(bias_backup_path, bias_path);
    if (weight_temporary[0] != '\0')
        unlink(weight_temporary);
    if (bias_temporary[0] != '\0')
        unlink(bias_temporary);
    free_weight_records(weights, weight_count);
    free_bias_records(biases, bias_count);
    if (snapshot != NULL)
        memset(snapshot, 0, sizeof(*snapshot));
    return -1;
}
