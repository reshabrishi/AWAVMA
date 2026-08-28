#include "migration.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CONFIG "config/awavma.conf"
#define DEFAULT_INPUT "results/validation_results.csv"
#define DEFAULT_RESULTS "results/migration_results.csv"
#define DEFAULT_HISTORY "history/migration_history.csv"
#define DEFAULT_LOG "logs/migration.log"
#define DEFAULT_STATE "state/migration_state.csv"
#define MAX_COLUMNS 64U

typedef struct {
    int timestamp;
    int migration_id;
    int app_id;
    int pid;
    int start_time_ticks;
    int tid;
    int entity_id;
    int action;
    int phase6_validation;
    int source_node;
    int destination_node;
    int destination_cpu;
    int page_count;
    int page_addresses;
    int page_metadata_verified;
    int explicit_placement;
    int memory_locked;
    int memory_pinned;
    size_t count;
} input_columns_t;

static void usage(const char *program)
{
    printf("Usage: %s [options]\n\n", program);
    printf("  --input FILE       Approved/rejected migration requests (default: %s)\n", DEFAULT_INPUT);
    printf("  --output FILE      Migration results CSV (default: %s)\n", DEFAULT_RESULTS);
    printf("  --history FILE     Migration history CSV (default: %s)\n", DEFAULT_HISTORY);
    printf("  --log FILE         Human migration log (default: %s)\n", DEFAULT_LOG);
    printf("  --state FILE       Current migration state (default: %s)\n", DEFAULT_STATE);
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
           strcmp(text, "-1") == 0 || strcmp(text, "UNKNOWN") == 0;
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

static int parse_int(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    if (unavailable(text))
        return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX)
        return -1;
    *value = (int)parsed;
    return 0;
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

static void defaults(MigrationConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->history_max_days = 30.0;
    config->history_max_records = 1000;
    config->history_decay_lambda = 0.1;
    config->cleanup_interval = 100;
    config->cooldown_ms = 1000;
    config->lock_timeout_ms = 0;
    config->verification_enabled = true;
}

static bool set_config_value(MigrationConfig *config, const char *key, const char *value)
{
    uint64_t integer;
    char *end = NULL;
    double number;
    bool boolean;

    if (strcmp(key, "migration_history_max_days") == 0 || strcmp(key, "migration_history_decay_lambda") == 0) {
        errno = 0;
        number = strtod(value, &end);
        if (errno != 0 || end == value || *end != '\0' || !isfinite(number) || number <= 0.0)
            return false;
        if (strcmp(key, "migration_history_max_days") == 0) config->history_max_days = number;
        else config->history_decay_lambda = number;
        return true;
    }
    if (strcmp(key, "migration_history_max_records") == 0 || strcmp(key, "migration_cleanup_interval") == 0 ||
        strcmp(key, "migration_cooldown_ms") == 0 || strcmp(key, "migration_lock_timeout_ms") == 0) {
        if (parse_u64(value, &integer) != 0 || integer > SIZE_MAX)
            return false;
        if (strcmp(key, "migration_history_max_records") == 0) config->history_max_records = (size_t)integer;
        else if (strcmp(key, "migration_cleanup_interval") == 0) config->cleanup_interval = (size_t)integer;
        else if (strcmp(key, "migration_cooldown_ms") == 0) {
            if (integer > UINT_MAX) return false;
            config->cooldown_ms = (unsigned)integer;
        } else {
            if (integer > UINT_MAX) return false;
            config->lock_timeout_ms = (unsigned)integer;
        }
        return true;
    }
    if (strcmp(key, "migration_verification_enabled") == 0)
        return parse_bool(value, &boolean) ? (config->verification_enabled = boolean, true) : false;
    return strncmp(key, "migration_", 10) != 0;
}

static bool load_config(const char *path, MigrationConfig *config)
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

static int prepare_columns(char **fields, size_t count, input_columns_t *columns)
{
    memset(columns, -1, sizeof(*columns));
    columns->count = count;
    columns->timestamp = column(fields, count, "timestamp");
    columns->migration_id = column(fields, count, "migration_id");
    columns->app_id = column(fields, count, "app_id");
    columns->pid = column(fields, count, "pid");
    columns->start_time_ticks = column(fields, count, "start_time_ticks");
    columns->tid = column(fields, count, "tid");
    columns->entity_id = column(fields, count, "entity_id");
    columns->action = column(fields, count, "action");
    columns->phase6_validation = column(fields, count, "phase6_validation");
    if (columns->phase6_validation < 0) columns->phase6_validation = column(fields, count, "final_decision");
    columns->source_node = column(fields, count, "source_node");
    columns->destination_node = column(fields, count, "destination_node");
    columns->destination_cpu = column(fields, count, "destination_cpu");
    columns->page_count = column(fields, count, "page_count");
    columns->page_addresses = column(fields, count, "page_addresses");
    columns->page_metadata_verified = column(fields, count, "memory_region_verified");
    if (columns->page_metadata_verified < 0) columns->page_metadata_verified = column(fields, count, "page_metadata_verified");
    columns->explicit_placement = column(fields, count, "explicit_placement_required");
    columns->memory_locked = column(fields, count, "memory_locked");
    columns->memory_pinned = column(fields, count, "memory_pinned");
    return columns->timestamp >= 0 && columns->pid >= 0 && columns->action >= 0 && columns->phase6_validation >= 0;
}

static bool parse_pages(char *text, size_t expected, void ***pages)
{
    void **parsed_pages;
    size_t count = 0;
    char *save = NULL;
    char *token;

    if (expected == 0 || unavailable(text))
        return false;
    parsed_pages = calloc(expected, sizeof(*parsed_pages));
    if (parsed_pages == NULL)
        return false;
    token = strtok_r(text, ";", &save);
    while (token != NULL && count < expected) {
        char *end = NULL;
        unsigned long long address;

        errno = 0;
        address = strtoull(token, &end, 0);
        if (errno != 0 || end == token || *end != '\0' || address == 0) {
            free(parsed_pages);
            return false;
        }
        parsed_pages[count++] = (void *)(uintptr_t)address;
        token = strtok_r(NULL, ";", &save);
    }
    if (count != expected || token != NULL) {
        free(parsed_pages);
        return false;
    }
    *pages = parsed_pages;
    return true;
}

static bool parse_row(char **fields, size_t count, const input_columns_t *columns,
                      const char *fallback_app, MigrationRequest *request)
{
    uint64_t number;
    int value;
    bool boolean;

    memset(request, 0, sizeof(*request));
    request->source_numa_node = -1;
    request->destination_numa_node = -1;
    request->destination_cpu = -1;
    if ((size_t)columns->pid >= count || parse_u64(fields[columns->pid], &number) != 0 ||
        parse_action(fields[columns->action], &request->phase5_decision.action) != 0)
        return false;
    request->pid = (pid_t)number;
    if (columns->start_time_ticks >= 0 && (size_t)columns->start_time_ticks < count &&
        !unavailable(fields[columns->start_time_ticks])) {
        if (parse_u64(fields[columns->start_time_ticks], &request->start_time_ticks) != 0)
            return false;
        request->start_time_ticks_available = true;
    }
    request->phase5_decision.pid = (long)request->pid;
    if (columns->tid >= 0 && (size_t)columns->tid < count && !unavailable(fields[columns->tid])) {
        if (parse_u64(fields[columns->tid], &number) != 0) return false;
        request->tid = (pid_t)number;
    }
    if (columns->app_id >= 0 && (size_t)columns->app_id < count && !unavailable(fields[columns->app_id]))
        snprintf(request->phase5_decision.app_id, sizeof(request->phase5_decision.app_id), "%s", fields[columns->app_id]);
    else if (fallback_app != NULL)
        snprintf(request->phase5_decision.app_id, sizeof(request->phase5_decision.app_id), "%s", fallback_app);
    else
        return false;
    if (columns->migration_id >= 0 && (size_t)columns->migration_id < count && !unavailable(fields[columns->migration_id]))
        snprintf(request->phase5_decision.migration_id, sizeof(request->phase5_decision.migration_id), "%s", fields[columns->migration_id]);
    else
        snprintf(request->phase5_decision.migration_id, sizeof(request->phase5_decision.migration_id), "UNKNOWN");
    if (columns->entity_id >= 0 && (size_t)columns->entity_id < count && !unavailable(fields[columns->entity_id]))
        snprintf(request->phase5_decision.entity_id, sizeof(request->phase5_decision.entity_id), "%s", fields[columns->entity_id]);
    else
        snprintf(request->phase5_decision.entity_id, sizeof(request->phase5_decision.entity_id), "workload");
    snprintf(request->phase5_decision.action_text, sizeof(request->phase5_decision.action_text), "%s",
             fields[columns->action]);
    snprintf(request->phase6_validation.final_decision, sizeof(request->phase6_validation.final_decision), "%s",
             fields[columns->phase6_validation]);
    snprintf(request->phase6_validation.validation_status, sizeof(request->phase6_validation.validation_status), "%s",
             fields[columns->phase6_validation]);
    if (columns->source_node >= 0 && (size_t)columns->source_node < count && parse_int(fields[columns->source_node], &value) == 0) {
        request->source_numa_node = value;
        request->numa_nodes_available = true;
    }
    if (columns->destination_node >= 0 && (size_t)columns->destination_node < count && parse_int(fields[columns->destination_node], &value) == 0) {
        request->destination_numa_node = value;
        request->numa_nodes_available = request->numa_nodes_available && true;
    } else {
        request->numa_nodes_available = false;
    }
    if (columns->destination_cpu >= 0 && (size_t)columns->destination_cpu < count && parse_int(fields[columns->destination_cpu], &value) == 0)
        request->destination_cpu = value;
    if (columns->page_count >= 0 && (size_t)columns->page_count < count && parse_u64(fields[columns->page_count], &number) == 0)
        request->page_count = (size_t)number;
    if (request->page_count > 0 && columns->page_addresses >= 0 && (size_t)columns->page_addresses < count &&
        !parse_pages(fields[columns->page_addresses], request->page_count, &request->pages))
        return false;
    request->page_metadata_available = request->pages != NULL && request->page_count > 0;
    if (columns->page_metadata_verified >= 0 && (size_t)columns->page_metadata_verified < count &&
        parse_bool(fields[columns->page_metadata_verified], &boolean))
        request->memory_region_verified = boolean;
    if (columns->explicit_placement >= 0 && (size_t)columns->explicit_placement < count &&
        parse_bool(fields[columns->explicit_placement], &boolean))
        request->explicit_placement_required = boolean;
    if (columns->memory_locked >= 0 && (size_t)columns->memory_locked < count &&
        parse_bool(fields[columns->memory_locked], &boolean))
        request->memory_locked = boolean;
    if (columns->memory_pinned >= 0 && (size_t)columns->memory_pinned < count &&
        parse_bool(fields[columns->memory_pinned], &boolean))
        request->memory_pinned = boolean;
    request->phase5_decision.nodes_available = request->numa_nodes_available;
    request->phase5_decision.source_node = request->source_numa_node;
    request->phase5_decision.destination_node = request->destination_numa_node;
    return true;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"input", required_argument, NULL, 'i'}, {"output", required_argument, NULL, 'o'},
        {"history", required_argument, NULL, 'H'}, {"log", required_argument, NULL, 'l'},
        {"state", required_argument, NULL, 's'}, {"config", required_argument, NULL, 'c'},
        {"app-id", required_argument, NULL, 'a'}, {"help", no_argument, NULL, '?'},
        {NULL, 0, NULL, 0}
    };
    MigrationConfig config;
    const char *input_path = DEFAULT_INPUT;
    const char *config_path = DEFAULT_CONFIG;
    const char *fallback_app = NULL;
    const char *output_path = DEFAULT_RESULTS;
    const char *history_path = DEFAULT_HISTORY;
    const char *log_path = DEFAULT_LOG;
    const char *state_path = DEFAULT_STATE;
    FILE *input;
    char *line = NULL;
    size_t line_capacity = 0;
    char *fields[MAX_COLUMNS];
    input_columns_t columns;
    ssize_t length;
    size_t rows = 0;
    size_t successful = 0;
    size_t refused = 0;
    int option;

    defaults(&config);
    while ((option = getopt_long(argc, argv, "i:o:H:l:s:c:a:?", options, NULL)) != -1) {
        switch (option) {
        case 'i': input_path = optarg; break;
        case 'o': output_path = optarg; break;
        case 'H': history_path = optarg; break;
        case 'l': log_path = optarg; break;
        case 's': state_path = optarg; break;
        case 'c': config_path = optarg; break;
        case 'a': fallback_app = optarg; break;
        case '?': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc || !load_config(config_path, &config)) {
        fprintf(stderr, "Error: invalid migration configuration or arguments.\n");
        return EXIT_FAILURE;
    }
    config.results_path = output_path;
    config.history_path = history_path;
    config.log_path = log_path;
    config.state_path = state_path;
    input = fopen(input_path, "r");
    if (input == NULL) {
        fprintf(stderr, "Error: cannot open migration input '%s': %s\n", input_path, strerror(errno));
        return EXIT_FAILURE;
    }
    length = getline(&line, &line_capacity, input);
    if (length < 0 || !prepare_columns(fields, split_csv(line, fields), &columns)) {
        fprintf(stderr, "Error: migration input header is invalid.\n");
        fclose(input); free(line); return EXIT_FAILURE;
    }
    if (!Migration_Init(&config)) {
        fprintf(stderr, "Error: migration configuration failed validation.\n");
        fclose(input); free(line); return EXIT_FAILURE;
    }
    while ((length = getline(&line, &line_capacity, input)) >= 0) {
        MigrationRequest request;
        MigrationReport report;
        MigrationResultCode result;
        (void)length;
        if (!parse_row(fields, split_csv(line, fields), &columns, fallback_app, &request)) {
            fprintf(stderr, "Error: invalid migration input row %zu.\n", rows + 2);
            Migration_Shutdown(); fclose(input); free(line); return EXIT_FAILURE;
        }
        result = Migration_Execute(&request, &report);
        free(request.pages);
        rows++;
        if (result == MIGRATION_SUCCESS || result == MIGRATION_PARTIAL_SUCCESS)
            successful++;
        else
            refused++;
    }
    fclose(input); free(line); Migration_Shutdown();
    printf("AWAVMA Migration Module\n");
    printf("-----------------------\n");
    printf("Input       : %s\n", input_path);
    printf("Rows        : %zu\n", rows);
    printf("Successful  : %zu\n", successful);
    printf("Not executed: %zu\n", refused);
    return EXIT_SUCCESS;
}
