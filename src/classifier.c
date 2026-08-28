#include "classifier.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#define MAX_CSV_COLUMNS 128U
#define DEFAULT_ENTITY "workload"

typedef struct {
    int timestamp;
    int elapsed_ms;
    int pid;
    int access;
    int entity;
    size_t count;
} csv_columns_t;

typedef struct {
    uint64_t elapsed_ms;
    double access_value;
} access_observation_t;

typedef struct {
    char *id;
    access_observation_t *observations;
    size_t count;
    size_t capacity;
    page_class_t classification;
    bool has_classification;
} entity_state_t;

static const char *classification_names[] = {
    "COLD", "MODERATE", "HOT", "NONE", "UNAVAILABLE"
};

void classifier_config_default(classifier_config_t *config)
{
    if (config == NULL)
        return;
    *config = (classifier_config_t){
        .input_path = "results/monitoring_results.csv",
        .output_path = "results/classification_results.csv",
        .access_column = "access_value",
        .entity_column = "entity_id",
        .window_size = 10U,
        .lambda = 0.1,
        .hot_threshold = 100.0,
        .moderate_threshold = 20.0,
        .hysteresis = 5.0,
        .require_access = false
    };
}

const char *classifier_class_name(page_class_t classification)
{
    if (classification < CLASS_COLD || classification > CLASS_UNAVAILABLE)
        return "UNKNOWN";
    return classification_names[classification];
}

static double monotonic_seconds(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
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

/* This project writes simple comma-separated fields without quoted commas. */
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

static int parse_u64_field(const char *text, uint64_t *value)
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

static int parse_pid_field(const char *text, long *pid)
{
    uint64_t parsed;

    if (parse_u64_field(text, &parsed) != 0 || parsed == 0 || parsed > LONG_MAX)
        return -1;
    *pid = (long)parsed;
    return 0;
}

static int parse_access_field(const char *text, double *value, bool *available)
{
    char *end = NULL;
    double parsed;

    if (text == NULL || *text == '\0') {
        *value = 0.0;
        *available = false;
        return 0;
    }
    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed))
        return -1;
    if (parsed < 0.0) {
        if (parsed == -1.0) {
            *value = 0.0;
            *available = false;
            return 0;
        }
        return -1;
    }
    *value = parsed;
    *available = true;
    return 0;
}

static char *duplicate_string(const char *value)
{
    size_t length = strlen(value) + 1;
    char *copy = malloc(length);

    if (copy != NULL)
        memcpy(copy, value, length);
    return copy;
}

static int find_entity(entity_state_t *entities, size_t count, const char *id)
{
    size_t index;

    for (index = 0; index < count; index++)
        if (strcmp(entities[index].id, id) == 0)
            return (int)index;
    return -1;
}

static int add_entity(entity_state_t **entities, size_t *count, const char *id,
                      size_t window_size)
{
    entity_state_t *expanded;
    char *copy;

    expanded = realloc(*entities, (*count + 1) * sizeof(**entities));
    if (expanded == NULL)
        return -1;
    *entities = expanded;
    copy = duplicate_string(id);
    if (copy == NULL)
        return -1;
    (*entities)[*count].id = copy;
    (*entities)[*count].observations = calloc(window_size, sizeof(access_observation_t));
    if ((*entities)[*count].observations == NULL) {
        free(copy);
        return -1;
    }
    (*entities)[*count].count = 0;
    (*entities)[*count].capacity = window_size;
    (*entities)[*count].classification = CLASS_COLD;
    (*entities)[*count].has_classification = false;
    (*count)++;
    return (int)(*count - 1);
}

static void free_entities(entity_state_t *entities, size_t count)
{
    size_t index;

    for (index = 0; index < count; index++) {
        free(entities[index].id);
        free(entities[index].observations);
    }
    free(entities);
}

static int add_observation(entity_state_t *entity, uint64_t elapsed_ms,
                           double access_value)
{
    if (entity->count == entity->capacity) {
        memmove(entity->observations, entity->observations + 1,
                (entity->capacity - 1) * sizeof(entity->observations[0]));
        entity->count--;
    }
    entity->observations[entity->count].elapsed_ms = elapsed_ms;
    entity->observations[entity->count].access_value = access_value;
    entity->count++;
    return 0;
}

static double calculate_score(const entity_state_t *entity, uint64_t current_ms,
                              double lambda)
{
    double score = 0.0;
    size_t index;

    for (index = 0; index < entity->count; index++) {
        double age_seconds = current_ms >= entity->observations[index].elapsed_ms
                                 ? (double)(current_ms - entity->observations[index].elapsed_ms) / 1000.0
                                 : 0.0;
        score += entity->observations[index].access_value * exp(-lambda * age_seconds);
    }
    return score;
}

static page_class_t classify_score(const entity_state_t *entity, double score,
                                   double hot_threshold, double moderate_threshold,
                                   double hysteresis)
{
    double hot_enter = hot_threshold + hysteresis;
    double moderate_enter = moderate_threshold + hysteresis;
    double hot_leave = hot_threshold - hysteresis;
    double moderate_leave = moderate_threshold - hysteresis;

    if (!entity->has_classification) {
        if (score >= hot_enter)
            return CLASS_HOT;
        if (score >= moderate_enter)
            return CLASS_MODERATE;
        return CLASS_COLD;
    }
    switch (entity->classification) {
    case CLASS_HOT:
        if (score >= hot_leave)
            return CLASS_HOT;
        if (score >= moderate_leave)
            return CLASS_MODERATE;
        return CLASS_COLD;
    case CLASS_MODERATE:
        if (score >= hot_enter)
            return CLASS_HOT;
        if (score < moderate_leave)
            return CLASS_COLD;
        return CLASS_MODERATE;
    case CLASS_COLD:
        if (score >= hot_enter)
            return CLASS_HOT;
        if (score >= moderate_enter)
            return CLASS_MODERATE;
        return CLASS_COLD;
    default:
        return CLASS_COLD;
    }
}

static const char *status_name(bool access_available)
{
    return access_available ? "CLASSIFIED" : "UNAVAILABLE_ACCESS_SIGNAL";
}

static void write_result(FILE *output, const char *timestamp, uint64_t elapsed_ms,
                         long pid, const entity_state_t *entity, double score,
                         page_class_t previous, page_class_t current,
                         const classifier_config_t *config, bool access_available)
{
    fprintf(output, "%s,%llu,%ld,%s,%.9f,%s,%s,%.9f,%zu,%.9f,%.9f,%.9f,%s\n",
            timestamp, (unsigned long long)elapsed_ms, pid, entity->id,
            access_available ? score : -1.0, classifier_class_name(previous),
            classifier_class_name(current), config->lambda, config->window_size,
            config->hot_threshold, config->moderate_threshold, config->hysteresis,
            status_name(access_available));
}

static int validate_config(const classifier_config_t *config)
{
    if (config == NULL || config->input_path == NULL || config->output_path == NULL ||
        config->access_column == NULL || config->entity_column == NULL)
        return -1;
    if (config->window_size == 0 || !isfinite(config->lambda) || config->lambda <= 0.0 ||
        !isfinite(config->hot_threshold) || !isfinite(config->moderate_threshold) ||
        config->hot_threshold <= config->moderate_threshold || config->moderate_threshold < 0.0 ||
        !isfinite(config->hysteresis) || config->hysteresis < 0.0)
        return -1;
    return 0;
}

int classifier_run(const classifier_config_t *config, classifier_summary_t *summary)
{
    FILE *input = NULL;
    FILE *output = NULL;
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t line_length;
    char *fields[MAX_CSV_COLUMNS];
    csv_columns_t columns;
    entity_state_t *entities = NULL;
    size_t entity_count = 0;
    long expected_pid = -1;
    uint64_t previous_elapsed = 0;
    bool have_elapsed = false;
    bool access_column_available;
    double start_time;
    int result = -1;

    if (summary != NULL)
        memset(summary, 0, sizeof(*summary));
    if (validate_config(config) != 0) {
        fprintf(stderr, "Error: invalid classifier configuration.\n");
        return -1;
    }
    start_time = monotonic_seconds();
    input = fopen(config->input_path, "r");
    if (input == NULL) {
        fprintf(stderr, "Error: cannot open classifier input '%s': %s\n",
                config->input_path, strerror(errno));
        return -1;
    }
    line_length = getline(&line, &line_capacity, input);
    if (line_length < 0) {
        fprintf(stderr, "Error: classifier input is empty.\n");
        goto cleanup;
    }
    columns.count = split_csv_line(line, fields);
    columns.timestamp = header_column(fields, columns.count, "timestamp");
    columns.elapsed_ms = header_column(fields, columns.count, "elapsed_ms");
    columns.pid = header_column(fields, columns.count, "pid");
    columns.access = header_column(fields, columns.count, config->access_column);
    columns.entity = header_column(fields, columns.count, config->entity_column);
    if (columns.timestamp < 0 || columns.elapsed_ms < 0 || columns.pid < 0) {
        fprintf(stderr, "Error: classifier input must contain timestamp, elapsed_ms, and pid columns.\n");
        goto cleanup;
    }
    access_column_available = columns.access >= 0;
    if (!access_column_available)
        fprintf(stderr, "Warning: access column '%s' is absent; classification will be marked unavailable.\n",
                config->access_column);
    output = fopen(config->output_path, "w");
    if (output == NULL) {
        fprintf(stderr, "Error: cannot open classifier output '%s': %s\n",
                config->output_path, strerror(errno));
        goto cleanup;
    }
    fprintf(output, "timestamp,elapsed_ms,pid,entity_id,score,previous_class,current_class,lambda,window_size,hot_threshold,moderate_threshold,hysteresis,status\n");
    while ((line_length = getline(&line, &line_capacity, input)) >= 0) {
        uint64_t elapsed_ms;
        long pid;
        double access_value = 0.0;
        bool access_available = false;
        const char *entity_id = DEFAULT_ENTITY;
        int entity_index;
        page_class_t previous_class;
        page_class_t current_class;
        double score = -1.0;

        (void)line_length;
        columns.count = split_csv_line(line, fields);
        if ((size_t)columns.timestamp >= columns.count || (size_t)columns.elapsed_ms >= columns.count ||
            (size_t)columns.pid >= columns.count) {
            fprintf(stderr, "Error: malformed CSV row %zu.\n", summary != NULL ? summary->rows + 2 : 0);
            goto cleanup;
        }
        if (fields[columns.timestamp][0] == '\0' ||
            parse_u64_field(fields[columns.elapsed_ms], &elapsed_ms) != 0 ||
            parse_pid_field(fields[columns.pid], &pid) != 0) {
            fprintf(stderr, "Error: invalid timestamp, elapsed_ms, or pid on CSV row %zu.\n",
                    summary != NULL ? summary->rows + 2 : 0);
            goto cleanup;
        }
        if (have_elapsed && elapsed_ms < previous_elapsed) {
            fprintf(stderr, "Error: elapsed_ms is not ordered on CSV row %zu.\n",
                    summary != NULL ? summary->rows + 2 : 0);
            goto cleanup;
        }
        previous_elapsed = elapsed_ms;
        have_elapsed = true;
        if (expected_pid < 0)
            expected_pid = pid;
        else if (expected_pid != pid) {
            fprintf(stderr, "Error: PID changed within classifier input.\n");
            goto cleanup;
        }
        if (columns.entity >= 0 && (size_t)columns.entity < columns.count && fields[columns.entity][0] != '\0')
            entity_id = fields[columns.entity];
        if (access_column_available) {
            if ((size_t)columns.access >= columns.count ||
                parse_access_field(fields[columns.access], &access_value, &access_available) != 0) {
                fprintf(stderr, "Error: invalid access value on CSV row %zu.\n",
                        summary != NULL ? summary->rows + 2 : 0);
                goto cleanup;
            }
        }
        entity_index = find_entity(entities, entity_count, entity_id);
        if (entity_index < 0)
            entity_index = add_entity(&entities, &entity_count, entity_id, config->window_size);
        if (entity_index < 0) {
            fprintf(stderr, "Error: unable to allocate classifier entity state.\n");
            goto cleanup;
        }
        previous_class = entities[entity_index].has_classification
                             ? entities[entity_index].classification : CLASS_NONE;
        if (access_available) {
            add_observation(&entities[entity_index], elapsed_ms, access_value);
            score = calculate_score(&entities[entity_index], elapsed_ms, config->lambda);
            current_class = classify_score(&entities[entity_index], score,
                                           config->hot_threshold, config->moderate_threshold,
                                           config->hysteresis);
            entities[entity_index].classification = current_class;
            entities[entity_index].has_classification = true;
            if (summary != NULL) {
                summary->classified_rows++;
                if (current_class == CLASS_HOT)
                    summary->hot_rows++;
                else if (current_class == CLASS_MODERATE)
                    summary->moderate_rows++;
                else if (current_class == CLASS_COLD)
                    summary->cold_rows++;
            }
        } else {
            current_class = CLASS_UNAVAILABLE;
            if (summary != NULL)
                summary->unavailable_rows++;
        }
        write_result(output, fields[columns.timestamp], elapsed_ms, pid,
                     &entities[entity_index], score, previous_class, current_class,
                     config, access_available);
        if (summary != NULL)
            summary->rows++;
    }
    if (ferror(input)) {
        fprintf(stderr, "Error: failed while reading classifier input.\n");
        goto cleanup;
    }
    if (summary == NULL || summary->rows == 0) {
        fprintf(stderr, "Error: classifier input contains no data rows.\n");
        goto cleanup;
    }
    if (config->require_access && (!access_column_available || summary->unavailable_rows > 0)) {
        fprintf(stderr, "Error: access observations are required but unavailable.\n");
        result = -2;
        goto cleanup;
    }
    if (summary != NULL)
        summary->elapsed_seconds = monotonic_seconds() - start_time;
    result = 0;

cleanup:
    free_entities(entities, entity_count);
    free(line);
    if (input != NULL)
        fclose(input);
    if (output != NULL)
        fclose(output);
    return result;
}
