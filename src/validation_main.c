#include "validation.h"
#include "monitor_profile.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEFAULT_CONFIG "config/awavma.conf"
#define DEFAULT_INPUT "results/decision_results.csv"
#define DEFAULT_RESULTS "results/validation_results.csv"
#define DEFAULT_HISTORY "history/validation_history.csv"
#define DEFAULT_LOG "logs/validation.log"
#define MAX_COLUMNS 128U

typedef struct {
    int timestamp;
    int migration_id;
    int app_id;
    int pid;
    int entity_id;
    int action;
    int decision_status;
    int classification;
    int classifier_confidence;
    int stability;
    int sample_count;
    int cpu;
    int memory;
    int concurrency;
    int source_node;
    int destination_node;
    int gain;
    int cost;
    int page_locked;
    int memory_pinned;
    int cooldown;
    int thread_locked;
    int in_progress;
    int max_migrations;
    size_t count;
} input_columns_t;

static void usage(const char *program)
{
    printf("Usage: %s [options]\n\n", program);
    printf("  --input FILE       Phase 5 decision CSV (default: %s)\n", DEFAULT_INPUT);
    printf("  --output FILE      Validation results CSV (default: %s)\n", DEFAULT_RESULTS);
    printf("  --history FILE     Validation history CSV (default: %s)\n", DEFAULT_HISTORY);
    printf("  --log FILE         Human validation log (default: %s)\n", DEFAULT_LOG);
    printf("  --config FILE      Configuration file (default: %s)\n", DEFAULT_CONFIG);
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

static bool unavailable(const char *text)
{
    return text == NULL || *text == '\0' || strcmp(text, "NA") == 0 || strcmp(text, "-1") == 0 || strcmp(text, "UNKNOWN") == 0;
}

static bool parse_bool(const char *text, bool *value)
{
    if (unavailable(text))
        return false;
    if (strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0) {
        *value = true;
        return true;
    }
    if (strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0) {
        *value = false;
        return true;
    }
    return false;
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

static void defaults(ValidationConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->stability_weight = 0.4;
    config->sample_weight = 0.3;
    config->classifier_weight = 0.3;
    config->gain_weight = 0.6;
    config->cost_weight = 0.4;
    config->cpu_weight = 0.34;
    config->memory_weight = 0.33;
    config->concurrency_weight = 0.33;
    config->confidence_weight = 0.30;
    config->roi_weight = 0.40;
    config->safety_weight = 0.30;
    config->confidence_threshold = 70.0;
    config->roi_threshold = 0.0;
    config->safety_threshold = 50.0;
    config->validation_threshold = 50.0;
    config->sample_reference_count = 10;
    config->history_max_days = 30.0;
    config->history_max_records = 1000;
    config->history_cleanup_interval = 100;
    config->history_decay_lambda = 0.1;
}

static bool set_config_value(ValidationConfig *config, const char *key, const char *value)
{
    double number;
    uint64_t integer;

    if (strncmp(key, "migration_", 10) == 0 || strncmp(key, "feedback_", 9) == 0)
        return true;
    if (parse_double(value, &number) != 0)
        return false;
    if (strcmp(key, "stability_weight") == 0) config->stability_weight = number;
    else if (strcmp(key, "sample_weight") == 0) config->sample_weight = number;
    else if (strcmp(key, "classifier_weight") == 0) config->classifier_weight = number;
    else if (strcmp(key, "gain_weight") == 0) config->gain_weight = number;
    else if (strcmp(key, "cost_weight") == 0) config->cost_weight = number;
    else if (strcmp(key, "cpu_weight") == 0) config->cpu_weight = number;
    else if (strcmp(key, "memory_weight") == 0) config->memory_weight = number;
    else if (strcmp(key, "concurrency_weight") == 0) config->concurrency_weight = number;
    else if (strcmp(key, "confidence_weight") == 0) config->confidence_weight = number;
    else if (strcmp(key, "roi_weight") == 0) config->roi_weight = number;
    else if (strcmp(key, "safety_weight") == 0) config->safety_weight = number;
    else if (strcmp(key, "confidence_threshold") == 0) config->confidence_threshold = number;
    else if (strcmp(key, "roi_threshold") == 0) config->roi_threshold = number;
    else if (strcmp(key, "safety_threshold") == 0) config->safety_threshold = number;
    else if (strcmp(key, "validation_threshold") == 0) config->validation_threshold = number;
    else if (strcmp(key, "history_max_days") == 0) config->history_max_days = number;
    else if (strcmp(key, "history_decay_lambda") == 0) config->history_decay_lambda = number;
    else if (strcmp(key, "sample_reference_count") == 0 || strcmp(key, "history_max_records") == 0 || strcmp(key, "history_cleanup_interval") == 0) {
        if (parse_u64(value, &integer) != 0 || integer == 0)
            return false;
        if (strcmp(key, "sample_reference_count") == 0) config->sample_reference_count = integer;
        else if (strcmp(key, "history_max_records") == 0) config->history_max_records = integer;
        else config->history_cleanup_interval = integer;
    } else {
        return false;
    }
    return true;
}

static bool load_config_file(const char *path, ValidationConfig *config)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL && errno == ENOENT)
        return true;
    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals;
        char *key;
        char *value;

        key = trim(line);
        if (*key == '\0' || *key == '#')
            continue;
        equals = strchr(key, '=');
        if (equals == NULL) {
            fclose(file);
            return false;
        }
        *equals = '\0';
        key = trim(key);
        value = trim(equals + 1);
        if (!set_config_value(config, key, value)) {
            fclose(file);
            return false;
        }
    }
    fclose(file);
    return true;
}

static bool parse_optional_double(char **fields, size_t count, int index, double *value, bool *available)
{
    if (index < 0 || (size_t)index >= count || unavailable(fields[index])) {
        *available = false;
        return true;
    }
    if (parse_double(fields[index], value) != 0) return false;
    *available = true;
    return true;
}

static bool parse_optional_bool(char **fields, size_t count, int index, bool *value, bool *available)
{
    if (index < 0 || (size_t)index >= count || unavailable(fields[index])) {
        *available = false;
        return true;
    }
    *available = parse_bool(fields[index], value);
    return *available;
}

static int prepare_columns(char **fields, size_t count, input_columns_t *columns)
{
    memset(columns, -1, sizeof(*columns));
    columns->count = count;
    columns->timestamp = column(fields, count, "timestamp");
    columns->migration_id = column(fields, count, "migration_id");
    columns->app_id = column(fields, count, "app_id");
    columns->pid = column(fields, count, "pid");
    columns->entity_id = column(fields, count, "entity_id");
    columns->action = column(fields, count, "action");
    if (columns->action < 0) columns->action = column(fields, count, "decision");
    columns->decision_status = column(fields, count, "decision_status");
    if (columns->decision_status < 0) columns->decision_status = column(fields, count, "status");
    columns->classification = column(fields, count, "classification");
    if (columns->classification < 0) columns->classification = column(fields, count, "current_class");
    columns->classifier_confidence = column(fields, count, "classifier_confidence");
    columns->stability = column(fields, count, "stability");
    columns->sample_count = column(fields, count, "sample_count");
    columns->cpu = column(fields, count, "cpu_utilization");
    columns->memory = column(fields, count, "memory_utilization");
    columns->concurrency = column(fields, count, "concurrency");
    columns->source_node = column(fields, count, "source_node");
    columns->destination_node = column(fields, count, "destination_node");
    columns->gain = column(fields, count, "predicted_gain");
    columns->cost = column(fields, count, "estimated_cost");
    columns->page_locked = column(fields, count, "page_locked");
    columns->memory_pinned = column(fields, count, "memory_pinned");
    columns->cooldown = column(fields, count, "cooldown_active");
    columns->thread_locked = column(fields, count, "thread_locked");
    columns->in_progress = column(fields, count, "migration_in_progress");
    columns->max_migrations = column(fields, count, "max_migrations_reached");
    return columns->timestamp >= 0 && columns->pid >= 0 && columns->action >= 0;
}

static bool parse_row(char **fields, size_t count, const input_columns_t *columns,
                      const char *fallback_app, MonitorData *monitor,
                      ClassifierData *classifier, DecisionData *decision,
                      char *timestamp, size_t timestamp_size)
{
    uint64_t samples;
    long pid;
    const char *app;
    bool available;

    if ((size_t)columns->timestamp >= count || (size_t)columns->pid >= count ||
        (size_t)columns->action >= count || fields[columns->timestamp][0] == '\0' ||
        parse_u64(fields[columns->pid], &samples) != 0)
        return false;
    pid = (long)samples;
    memset(monitor, 0, sizeof(*monitor));
    memset(classifier, 0, sizeof(*classifier));
    memset(decision, 0, sizeof(*decision));
    decision->pid = pid;
    if (parse_action(fields[columns->action], &decision->action) != 0)
        return false;
    snprintf(decision->action_text, sizeof(decision->action_text), "%s", fields[columns->action]);
    if (columns->decision_status >= 0 && (size_t)columns->decision_status < count)
        snprintf(decision->status_text, sizeof(decision->status_text), "%s", fields[columns->decision_status]);
    else
        snprintf(decision->status_text, sizeof(decision->status_text), "UNKNOWN");
    if (strcasecmp(decision->status_text, "INSUFFICIENT_DECISION_SIGNAL") == 0)
        decision->action = VALIDATION_ACTION_INSUFFICIENT;
    app = columns->app_id >= 0 && (size_t)columns->app_id < count && !unavailable(fields[columns->app_id])
              ? fields[columns->app_id] : fallback_app;
    if (app == NULL || *app == '\0') return false;
    snprintf(decision->app_id, sizeof(decision->app_id), "%s", app);
    snprintf(decision->migration_id, sizeof(decision->migration_id), "%s",
             columns->migration_id >= 0 && (size_t)columns->migration_id < count && !unavailable(fields[columns->migration_id]) ? fields[columns->migration_id] : "UNKNOWN");
    snprintf(decision->entity_id, sizeof(decision->entity_id), "%s",
             columns->entity_id >= 0 && (size_t)columns->entity_id < count && !unavailable(fields[columns->entity_id]) ? fields[columns->entity_id] : "workload");
    snprintf(timestamp, timestamp_size, "%s", fields[columns->timestamp]);
    decision->nodes_available = columns->source_node >= 0 && columns->destination_node >= 0 &&
                                (size_t)columns->source_node < count && (size_t)columns->destination_node < count &&
                                !unavailable(fields[columns->source_node]) && !unavailable(fields[columns->destination_node]);
    if (decision->nodes_available) {
        long source, destination;
        if (parse_u64(fields[columns->source_node], &samples) != 0 || parse_u64(fields[columns->destination_node], &samples) != 0)
            decision->nodes_available = false;
        else {
            source = strtol(fields[columns->source_node], NULL, 10);
            destination = strtol(fields[columns->destination_node], NULL, 10);
            decision->source_node = (int)source;
            decision->destination_node = (int)destination;
        }
    }
    if (!parse_optional_double(fields, count, columns->gain, &decision->predicted_gain, &decision->gain_available) ||
        !parse_optional_double(fields, count, columns->cost, &decision->estimated_cost, &decision->cost_available))
        return false;
    if (columns->classification >= 0 && (size_t)columns->classification < count && !unavailable(fields[columns->classification])) {
        classifier->available = strcasecmp(fields[columns->classification], "HOT") == 0 ||
                                strcasecmp(fields[columns->classification], "MODERATE") == 0 ||
                                strcasecmp(fields[columns->classification], "COLD") == 0;
        snprintf(classifier->classification, sizeof(classifier->classification), "%s", fields[columns->classification]);
    }
    if (parse_optional_double(fields, count, columns->classifier_confidence, &classifier->confidence, &available) && available)
        classifier->available = classifier->available && classifier->confidence >= 0.0 && classifier->confidence <= 100.0;
    if (!parse_optional_double(fields, count, columns->stability, &monitor->stability, &available)) return false;
    bool stability_available = available;
    bool sample_available = columns->sample_count >= 0 && (size_t)columns->sample_count < count && !unavailable(fields[columns->sample_count]);
    if (sample_available && parse_u64(fields[columns->sample_count], &monitor->sample_count) != 0) return false;
    if (!parse_optional_double(fields, count, columns->cpu, &monitor->cpu_utilization, &available)) return false;
    bool cpu_available = available;
    if (!parse_optional_double(fields, count, columns->memory, &monitor->memory_utilization, &available)) return false;
    bool memory_available = available;
    if (!parse_optional_double(fields, count, columns->concurrency, &monitor->concurrency_score, &available)) return false;
    bool concurrency_available = available;
    monitor->available = stability_available && sample_available && cpu_available && memory_available && concurrency_available && monitor->stability >= 0.0 && monitor->stability <= 100.0;
    monitor->hard_constraints_available = true;
    bool hard_available;
    if (!parse_optional_bool(fields, count, columns->page_locked, &monitor->page_locked, &hard_available)) return false;
    monitor->hard_constraints_available = monitor->hard_constraints_available && hard_available;
    if (!parse_optional_bool(fields, count, columns->memory_pinned, &monitor->memory_pinned, &hard_available)) return false;
    monitor->hard_constraints_available = monitor->hard_constraints_available && hard_available;
    if (!parse_optional_bool(fields, count, columns->cooldown, &monitor->cooldown_active, &hard_available)) return false;
    monitor->hard_constraints_available = monitor->hard_constraints_available && hard_available;
    if (!parse_optional_bool(fields, count, columns->thread_locked, &monitor->thread_locked, &hard_available)) return false;
    monitor->hard_constraints_available = monitor->hard_constraints_available && hard_available;
    if (!parse_optional_bool(fields, count, columns->in_progress, &monitor->migration_in_progress, &hard_available)) return false;
    monitor->hard_constraints_available = monitor->hard_constraints_available && hard_available;
    if (!parse_optional_bool(fields, count, columns->max_migrations, &monitor->max_migrations_reached, &hard_available)) return false;
    monitor->hard_constraints_available = monitor->hard_constraints_available && hard_available;
    return true;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"input", required_argument, NULL, 'i'}, {"output", required_argument, NULL, 'o'},
        {"history", required_argument, NULL, 'H'}, {"log", required_argument, NULL, 'l'},
        {"config", required_argument, NULL, 'c'}, {"app-id", required_argument, NULL, 'a'},
        {"help", no_argument, NULL, '?'}, {NULL, 0, NULL, 0}
    };
    ValidationConfig config;
    const char *config_path = DEFAULT_CONFIG;
    const char *input_path = DEFAULT_INPUT;
    const char *fallback_app = NULL;
    char *output_path = (char *)DEFAULT_RESULTS;
    char *history_path = (char *)DEFAULT_HISTORY;
    char *log_path = (char *)DEFAULT_LOG;
    FILE *input = NULL;
    char *line = NULL;
    size_t line_capacity = 0;
    char *fields[MAX_COLUMNS];
    input_columns_t columns;
    ssize_t length;
    size_t rows = 0, pass = 0, rejected = 0;
    int option;
    monitor_profile_scope_t input_profile;
    monitor_profile_scope_t init_profile;

    defaults(&config);
    while ((option = getopt_long(argc, argv, "i:o:H:l:c:a:?", options, NULL)) != -1) {
        switch (option) {
        case 'i': input_path = optarg; break;
        case 'o': output_path = optarg; break;
        case 'H': history_path = optarg; break;
        case 'l': log_path = optarg; break;
        case 'c': config_path = optarg; break;
        case 'a': fallback_app = optarg; break;
        case '?': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc || !load_config_file(config_path, &config)) {
        fprintf(stderr, "Error: invalid validation configuration or arguments.\n");
        return EXIT_FAILURE;
    }
    config.results_path = output_path;
    config.history_path = history_path;
    config.log_path = log_path;
    monitor_profile_scope_begin(&input_profile, "phase56_child", "p6_input_open_parse", fallback_app, -1, 0);
    input = fopen(input_path, "r");
    if (input == NULL) {
        fprintf(stderr, "Error: cannot open validation input '%s': %s\n", input_path, strerror(errno));
        monitor_profile_scope_end(&input_profile, "ERROR");
        return EXIT_FAILURE;
    }
    length = getline(&line, &line_capacity, input);
    if (length < 0 || !prepare_columns(fields, split_csv(line, fields), &columns)) {
        fprintf(stderr, "Error: validation input header is invalid.\n");
        monitor_profile_scope_end(&input_profile, "ERROR");
        fclose(input); free(line); return EXIT_FAILURE;
    }
    monitor_profile_scope_end(&input_profile, "OK");
    monitor_profile_scope_begin(&init_profile, "phase56_child", "p6_initialize", fallback_app, -1, 0);
    if (!Validation_Init(&config)) {
        fprintf(stderr, "Error: validation configuration failed validation.\n");
        monitor_profile_scope_end(&init_profile, "ERROR");
        fclose(input); free(line); return EXIT_FAILURE;
    }
    monitor_profile_scope_end(&init_profile, "OK");
    while ((length = getline(&line, &line_capacity, input)) >= 0) {
        MonitorData monitor;
        ClassifierData classifier;
        DecisionData decision;
        ValidationResult result;
        char timestamp[32];
        monitor_profile_scope_t profile;
        (void)length;
        monitor_profile_scope_t parse_profile;
        monitor_profile_scope_begin(&parse_profile, "phase56_child", "p6_input_row_parse", fallback_app, -1, 0);
        if (!parse_row(fields, split_csv(line, fields), &columns, fallback_app,
                       &monitor, &classifier, &decision, timestamp, sizeof(timestamp))) {
            fprintf(stderr, "Error: invalid validation input row %zu.\n", rows + 2);
            monitor_profile_scope_end(&parse_profile, "ERROR");
            Validation_Shutdown(); fclose(input); free(line); return EXIT_FAILURE;
        }
        monitor_profile_scope_end(&parse_profile, "OK");
        monitor_profile_scope_begin(&profile, "phase46_child", "phase6_child_validate_log", decision.app_id,
                                     decision.pid, 0);
        monitor_profile_scope_t validate_profile;
        monitor_profile_scope_begin(&validate_profile, "phase56_child", "p6_validate_total", decision.app_id,
                                    decision.pid, 0);
        result = ValidateMigration(&monitor, &decision, &classifier);
        monitor_profile_scope_end(&validate_profile, "OK");
        snprintf(result.timestamp, sizeof(result.timestamp), "%s", timestamp);
        monitor_profile_scope_t log_profile;
        monitor_profile_scope_begin(&log_profile, "phase56_child", "p6_logging_total", decision.app_id,
                                    decision.pid, 0);
        if (!LogValidationResult(&result, &decision)) {
            monitor_profile_scope_end(&log_profile, "ERROR");
            monitor_profile_scope_end(&profile, "ERROR");
            fprintf(stderr, "Error: validation result logging failed.\n");
            Validation_Shutdown(); fclose(input); free(line); return EXIT_FAILURE;
        }
        monitor_profile_scope_end(&log_profile, "OK");
        monitor_profile_scope_end(&profile, "OK");
        rows++;
        if (strcmp(result.final_decision, "APPROVED") == 0 || strcmp(result.final_decision, "NO_MIGRATION") == 0)
            pass++;
        else
            rejected++;
    }
    fclose(input); free(line);
    monitor_profile_scope_t shutdown_profile;
    monitor_profile_scope_begin(&shutdown_profile, "phase56_child", "p6_shutdown", fallback_app, -1, 0);
    Validation_Shutdown();
    monitor_profile_scope_end(&shutdown_profile, "OK");
    printf("AWAVMA Validation Module\n");
    printf("------------------------\n");
    printf("Input       : %s\n", input_path);
    printf("Rows        : %zu\n", rows);
    printf("Accepted    : %zu\n", pass);
    printf("Rejected    : %zu\n", rejected);
    printf("Migration was not executed.\n");
    return EXIT_SUCCESS;
}
