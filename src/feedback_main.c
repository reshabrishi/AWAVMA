#include "feedback.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CONFIG "config/awavma.conf"
#define DEFAULT_INPUT "results/feedback_inputs.csv"
#define DEFAULT_RESULTS "results/feedback_results.csv"
#define DEFAULT_STATE_DIR "state"
#define DEFAULT_HISTORY_DIR "history"
#define DEFAULT_HISTORY "history/feedback_history.csv"
#define DEFAULT_LOG "logs/feedback.log"
#define MAX_COLUMNS 64U

typedef struct {
    int timestamp;
    int feedback_id;
    int migration_id;
    int app_id;
    int pid;
    int entity_id;
    int action;
    int phase6_validation;
    int migration_result;
    int verification_status;
    int before_metric[FEEDBACK_METRIC_COUNT];
    int after_metric[FEEDBACK_METRIC_COUNT];
    int before_samples;
    int after_samples;
    int confidence;
    int recorded_epoch;
    int factor[FEEDBACK_SIGNAL_COUNT];
    int migration_execution_time;
    int pages_migrated;
    int pages_failed;
} input_columns_t;

static void usage(const char *program)
{
    printf("Usage: %s [options]\n\n", program);
    printf("  --input FILE       Feedback event CSV (default: %s)\n", DEFAULT_INPUT);
    printf("  --output FILE      Machine-readable feedback results (default: %s)\n", DEFAULT_RESULTS);
    printf("  --history FILE     Feedback history CSV (default: %s)\n", DEFAULT_HISTORY);
    printf("  --log FILE         Human feedback log (default: %s)\n", DEFAULT_LOG);
    printf("  --state-dir DIR    Phase 5 state directory (default: %s)\n", DEFAULT_STATE_DIR);
    printf("  --history-dir DIR  Phase 5 history directory (default: %s)\n", DEFAULT_HISTORY_DIR);
    printf("  --config FILE      AWAVMA configuration (default: %s)\n", DEFAULT_CONFIG);
    printf("  --app-id ID        Application ID when input has no app_id\n");
    printf("  --help             Show this help\n");
}

static char *trim(char *value)
{
    char *end;

    while (isspace((unsigned char)*value))
        value++;
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return value;
}

static size_t split_csv(char *line, char **fields)
{
    size_t count = 0;
    char *start = line;
    char *cursor = line;

    while (*cursor != '\0') {
        if (*cursor == ',') {
            *cursor = '\0';
            if (count < MAX_COLUMNS)
                fields[count++] = trim(start);
            start = cursor + 1;
        } else if (*cursor == '\n' || *cursor == '\r') {
            *cursor = '\0';
            if (count < MAX_COLUMNS)
                fields[count++] = trim(start);
            return count;
        }
        cursor++;
    }
    if (count < MAX_COLUMNS)
        fields[count++] = trim(start);
    return count;
}

static int column(char **fields, size_t count, const char *name)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(fields[index], name) == 0)
            return (int)index;
    return -1;
}

static bool unavailable(const char *text)
{
    return text == NULL || *text == '\0' || strcmp(text, "NA") == 0 ||
           strcmp(text, "-1") == 0 || strcmp(text, "-1.0") == 0 || strcmp(text, "UNKNOWN") == 0;
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (unavailable(text) || text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_double(const char *text, double *value)
{
    char *end = NULL;
    double parsed;

    if (unavailable(text))
        return -1;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed))
        return -1;
    *value = parsed;
    return 0;
}

static int parse_action(const char *text, ValidationAction *action)
{
    if (strcasecmp(text, "MOVE_MEMORY") == 0) *action = VALIDATION_ACTION_MOVE_MEMORY;
    else if (strcasecmp(text, "MOVE_THREAD") == 0) *action = VALIDATION_ACTION_MOVE_THREAD;
    else if (strcasecmp(text, "NO_MIGRATION") == 0) *action = VALIDATION_ACTION_NO_MIGRATION;
    else if (strcasecmp(text, "INSUFFICIENT_DECISION_SIGNAL") == 0) *action = VALIDATION_ACTION_INSUFFICIENT;
    else return -1;
    return 0;
}

static void defaults(FeedbackConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->state_dir = DEFAULT_STATE_DIR;
    config->history_dir = DEFAULT_HISTORY_DIR;
    config->history_max_days = 30.0;
    config->history_max_records = 1000;
    config->cleanup_interval = 100;
    config->learning_rate = 0.05;
    config->bias_learning_rate = 0.05;
    config->min_samples = 5;
    config->deadband = 0.05;
    config->reward_min = -1.0;
    config->reward_max = 1.0;
    config->decay_lambda = 0.1;
    config->min_confidence = 0.5;
    config->min_weight = 0.0;
    config->max_weight = 2.0;
    config->min_bias = -0.25;
    config->max_bias = 0.25;
    config->throughput_weight = 0.4;
    config->execution_time_weight = 0.2;
    config->latency_weight = 0.2;
    config->page_fault_weight = 0.2;
}

static bool set_config_value(FeedbackConfig *config, const char *key, const char *value)
{
    char *end = NULL;
    double number;
    uint64_t integer;

#define SET_DOUBLE(name, field, minimum) \
    if (strcmp(key, name) == 0) { \
        errno = 0; number = strtod(value, &end); \
        if (errno != 0 || end == value || *end != '\0' || !isfinite(number) || number < minimum) return false; \
        config->field = number; return true; \
    }
#define SET_INTEGER(name, field) \
    if (strcmp(key, name) == 0) { \
        if (parse_u64(value, &integer) != 0 || integer > SIZE_MAX) return false; \
        config->field = (size_t)integer; return true; \
    }

    SET_DOUBLE("feedback_learning_rate", learning_rate, 0.0)
    SET_DOUBLE("feedback_bias_learning_rate", bias_learning_rate, 0.0)
    SET_INTEGER("feedback_min_samples", min_samples)
    SET_DOUBLE("feedback_deadband", deadband, 0.0)
    SET_DOUBLE("feedback_reward_min", reward_min, -1.0)
    SET_DOUBLE("feedback_reward_max", reward_max, -1.0)
    SET_DOUBLE("feedback_decay_lambda", decay_lambda, 0.0)
    SET_DOUBLE("feedback_min_confidence", min_confidence, 0.0)
    SET_INTEGER("feedback_history_max_records", history_max_records)
    SET_DOUBLE("feedback_history_max_days", history_max_days, 0.0)
    SET_INTEGER("feedback_cleanup_interval", cleanup_interval)
    SET_DOUBLE("feedback_min_weight", min_weight, 0.0)
    SET_DOUBLE("feedback_max_weight", max_weight, 0.0)
    SET_DOUBLE("feedback_min_bias", min_bias, -1.0)
    SET_DOUBLE("feedback_max_bias", max_bias, -1.0)
    SET_DOUBLE("feedback_throughput_weight", throughput_weight, 0.0)
    SET_DOUBLE("feedback_execution_time_weight", execution_time_weight, 0.0)
    SET_DOUBLE("feedback_latency_weight", latency_weight, 0.0)
    SET_DOUBLE("feedback_page_fault_weight", page_fault_weight, 0.0)
    return strncmp(key, "feedback_", 9) != 0;
#undef SET_DOUBLE
#undef SET_INTEGER
}

static bool load_config(const char *path, FeedbackConfig *config)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL && errno == ENOENT)
        return true;
    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *key = trim(line);
        char *equals;

        if (*key == '\0' || *key == '#')
            continue;
        equals = strchr(key, '=');
        if (equals == NULL) {
            fclose(file);
            return false;
        }
        *equals = '\0';
        if (!set_config_value(config, trim(key), trim(equals + 1))) {
            fclose(file);
            return false;
        }
    }
    fclose(file);
    return true;
}

static void prepare_columns(char **fields, size_t count, input_columns_t *columns)
{
    memset(columns, -1, sizeof(*columns));
    columns->timestamp = column(fields, count, "timestamp");
    columns->feedback_id = column(fields, count, "feedback_id");
    columns->migration_id = column(fields, count, "migration_id");
    columns->app_id = column(fields, count, "app_id");
    columns->pid = column(fields, count, "pid");
    columns->entity_id = column(fields, count, "entity_id");
    columns->action = column(fields, count, "action");
    columns->phase6_validation = column(fields, count, "phase6_validation");
    columns->migration_result = column(fields, count, "migration_result");
    columns->verification_status = column(fields, count, "verification_status");
    columns->before_metric[FEEDBACK_METRIC_THROUGHPUT] = column(fields, count, "before_throughput");
    columns->after_metric[FEEDBACK_METRIC_THROUGHPUT] = column(fields, count, "after_throughput");
    columns->before_metric[FEEDBACK_METRIC_EXECUTION_TIME] = column(fields, count, "before_execution_time");
    columns->after_metric[FEEDBACK_METRIC_EXECUTION_TIME] = column(fields, count, "after_execution_time");
    columns->before_metric[FEEDBACK_METRIC_LATENCY] = column(fields, count, "before_latency");
    columns->after_metric[FEEDBACK_METRIC_LATENCY] = column(fields, count, "after_latency");
    columns->before_metric[FEEDBACK_METRIC_PAGE_FAULTS] = column(fields, count, "before_page_faults");
    columns->after_metric[FEEDBACK_METRIC_PAGE_FAULTS] = column(fields, count, "after_page_faults");
    columns->before_samples = column(fields, count, "before_samples");
    columns->after_samples = column(fields, count, "after_samples");
    columns->confidence = column(fields, count, "measurement_confidence");
    columns->recorded_epoch = column(fields, count, "recorded_at_epoch");
    columns->migration_execution_time = column(fields, count, "migration_execution_time_ms");
    columns->pages_migrated = column(fields, count, "pages_migrated");
    columns->pages_failed = column(fields, count, "pages_failed");
    for (int index = 0; index < (int)FEEDBACK_SIGNAL_COUNT; index++) {
        const char *action = index < (int)DECISION_FACTOR_COUNT ? "memory" : "thread";
        const char *factor = decision_factor_name((decision_factor_t)(index % DECISION_FACTOR_COUNT));
        char name[64];

        if (index % DECISION_FACTOR_COUNT == FACTOR_ACCESS)
            snprintf(name, sizeof(name), "f_access");
        else if (index % DECISION_FACTOR_COUNT == FACTOR_THRESHOLD)
            snprintf(name, sizeof(name), "f_threshold");
        else
            snprintf(name, sizeof(name), "f_%s_%s", factor, action);
        columns->factor[index] = column(fields, count, name);
    }
}

static bool field_at(char **fields, size_t count, int index, const char **value)
{
    if (index < 0 || (size_t)index >= count || unavailable(fields[index])) {
        *value = NULL;
        return false;
    }
    *value = fields[index];
    return true;
}

static bool parse_event(char **fields, size_t count, const input_columns_t *columns,
                        const char *fallback_app, FeedbackEvent *event)
{
    const char *value;
    uint64_t integer;
    double number;

    memset(event, 0, sizeof(*event));
    if (!field_at(fields, count, columns->action, &value) ||
        parse_action(value, &event->action) != 0 ||
        !field_at(fields, count, columns->phase6_validation, &value))
        return false;
    snprintf(event->phase6_validation, sizeof(event->phase6_validation), "%s", value);
    if (!field_at(fields, count, columns->migration_result, &value))
        return false;
    snprintf(event->migration_result, sizeof(event->migration_result), "%s", value);
    if (!field_at(fields, count, columns->pid, &value) || parse_u64(value, &integer) != 0)
        return false;
    event->pid = (long)integer;
    if (columns->timestamp >= 0 && (size_t)columns->timestamp < count)
        snprintf(event->timestamp, sizeof(event->timestamp), "%s", fields[columns->timestamp]);
    else
        snprintf(event->timestamp, sizeof(event->timestamp), "UNKNOWN");
    if (columns->feedback_id >= 0 && field_at(fields, count, columns->feedback_id, &value))
        snprintf(event->feedback_id, sizeof(event->feedback_id), "%s", value);
    else
        snprintf(event->feedback_id, sizeof(event->feedback_id), "UNKNOWN");
    if (columns->migration_id >= 0 && field_at(fields, count, columns->migration_id, &value))
        snprintf(event->migration_id, sizeof(event->migration_id), "%s", value);
    else
        snprintf(event->migration_id, sizeof(event->migration_id), "UNKNOWN");
    if (columns->app_id >= 0 && field_at(fields, count, columns->app_id, &value))
        snprintf(event->app_id, sizeof(event->app_id), "%s", value);
    else if (fallback_app != NULL)
        snprintf(event->app_id, sizeof(event->app_id), "%s", fallback_app);
    else
        return false;
    if (columns->entity_id >= 0 && field_at(fields, count, columns->entity_id, &value))
        snprintf(event->entity_id, sizeof(event->entity_id), "%s", value);
    else
        snprintf(event->entity_id, sizeof(event->entity_id), "workload");
    for (int metric = 0; metric < (int)FEEDBACK_METRIC_COUNT; metric++) {
        const char *before;
        const char *after;

        if (!field_at(fields, count, columns->before_metric[metric], &before) ||
            !field_at(fields, count, columns->after_metric[metric], &after))
            continue;
        if (parse_double(before, &event->before_metrics[metric]) != 0 ||
            parse_double(after, &event->after_metrics[metric]) != 0)
            return false;
        event->metric_available[metric] = true;
    }
    if (field_at(fields, count, columns->before_samples, &value) &&
        parse_u64(value, &integer) == 0) {
        event->before_samples = (size_t)integer;
        if (field_at(fields, count, columns->after_samples, &value) && parse_u64(value, &integer) == 0) {
            event->after_samples = (size_t)integer;
            event->sample_counts_available = true;
        }
    }
    if (field_at(fields, count, columns->confidence, &value)) {
        if (parse_double(value, &number) != 0) return false;
        event->supplied_confidence = number;
        event->supplied_confidence_available = true;
    }
    if (field_at(fields, count, columns->recorded_epoch, &value) && parse_u64(value, &integer) == 0) {
        event->recorded_at_epoch = (long long)integer;
        event->epoch_available = true;
    }
    if (field_at(fields, count, columns->migration_execution_time, &value)) {
        if (parse_double(value, &number) != 0 || number < 0.0) return false;
        event->migration_execution_time_ms = number;
        event->migration_cost_available = true;
    }
    if (field_at(fields, count, columns->pages_migrated, &value) && parse_u64(value, &integer) == 0)
        event->pages_migrated = (size_t)integer;
    if (field_at(fields, count, columns->pages_failed, &value) && parse_u64(value, &integer) == 0)
        event->pages_failed = (size_t)integer;
    if (field_at(fields, count, columns->verification_status, &value)) {
        event->verification_available = true;
        event->verification_succeeded = strcasecmp(value, "VERIFIED") == 0 ||
                                        strcasecmp(value, "SUCCESS") == 0;
    }
    for (int index = 0; index < (int)FEEDBACK_SIGNAL_COUNT; index++) {
        if (!field_at(fields, count, columns->factor[index], &value))
            continue;
        if (parse_double(value, &event->factor_signals[index]) != 0)
            return false;
        event->factor_available[index] = true;
    }
    return true;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"input", required_argument, NULL, 'i'}, {"output", required_argument, NULL, 'o'},
        {"history", required_argument, NULL, 'H'},
        {"log", required_argument, NULL, 'l'}, {"state-dir", required_argument, NULL, 's'},
        {"history-dir", required_argument, NULL, 'd'}, {"config", required_argument, NULL, 'c'},
        {"app-id", required_argument, NULL, 'a'}, {"help", no_argument, NULL, '?'},
        {NULL, 0, NULL, 0}
    };
    FeedbackConfig config;
    const char *input_path = DEFAULT_INPUT;
    const char *results_path = DEFAULT_RESULTS;
    const char *config_path = DEFAULT_CONFIG;
    const char *fallback_app = NULL;
    const char *history_path = DEFAULT_HISTORY;
    const char *log_path = DEFAULT_LOG;
    const char *state_dir = DEFAULT_STATE_DIR;
    const char *history_dir = DEFAULT_HISTORY_DIR;
    FILE *input;
    char *line = NULL;
    size_t capacity = 0;
    char *fields[MAX_COLUMNS];
    input_columns_t columns;
    ssize_t length;
    size_t rows = 0;
    size_t updated = 0;
    size_t not_updated = 0;
    int option;

    defaults(&config);
    while ((option = getopt_long(argc, argv, "i:o:H:l:s:d:c:a:?", options, NULL)) != -1) {
        switch (option) {
        case 'i': input_path = optarg; break;
        case 'o': results_path = optarg; break;
        case 'H': history_path = optarg; break;
        case 'l': log_path = optarg; break;
        case 's': state_dir = optarg; break;
        case 'd': history_dir = optarg; break;
        case 'c': config_path = optarg; break;
        case 'a': fallback_app = optarg; break;
        case '?': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc || !load_config(config_path, &config)) {
        fprintf(stderr, "Error: invalid feedback configuration or arguments.\n");
        return EXIT_FAILURE;
    }
    config.state_dir = state_dir;
    config.history_dir = history_dir;
    config.results_path = results_path;
    config.history_path = history_path;
    config.log_path = log_path;
    input = fopen(input_path, "r");
    if (input == NULL) {
        fprintf(stderr, "Error: cannot open feedback input '%s': %s\n", input_path, strerror(errno));
        return EXIT_FAILURE;
    }
    length = getline(&line, &capacity, input);
    if (length < 0) {
        fclose(input); free(line); return EXIT_FAILURE;
    }
    prepare_columns(fields, split_csv(line, fields), &columns);
    if (columns.action < 0 || columns.phase6_validation < 0 || columns.migration_result < 0 || columns.pid < 0) {
        fprintf(stderr, "Error: feedback input header is missing required fields.\n");
        fclose(input); free(line); return EXIT_FAILURE;
    }
    if (!Feedback_Init(&config)) {
        fprintf(stderr, "Error: feedback configuration failed validation.\n");
        fclose(input); free(line); return EXIT_FAILURE;
    }
    while ((length = getline(&line, &capacity, input)) >= 0) {
        FeedbackEvent event;
        FeedbackResult result;
        FeedbackUpdateStatus status;
        (void)length;
        if (!parse_event(fields, split_csv(line, fields), &columns, fallback_app, &event)) {
            fprintf(stderr, "Error: invalid feedback row %zu.\n", rows + 2);
            Feedback_Shutdown(); fclose(input); free(line); return EXIT_FAILURE;
        }
        status = ProcessFeedback(&event, &result);
        rows++;
        if (status == FEEDBACK_UPDATED) updated++;
        else not_updated++;
    }
    fclose(input); free(line); Feedback_Shutdown();
    printf("AWAVMA Feedback Learning Module\n");
    printf("-------------------------------\n");
    printf("Input       : %s\n", input_path);
    printf("Rows        : %zu\n", rows);
    printf("Updated     : %zu\n", updated);
    printf("Not updated : %zu\n", not_updated);
    return EXIT_SUCCESS;
}
