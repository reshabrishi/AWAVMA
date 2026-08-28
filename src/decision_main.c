#include "decision.h"
#include "monitor_profile.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_INPUT "results/classification_results.csv"
#define DEFAULT_OUTPUT "results/decision_results.csv"
#define DEFAULT_STATE_DIR "state"
#define DEFAULT_HISTORY_DIR "history"
#define DEFAULT_LOG "logs/decision_engine.log"
#define DEFAULT_EPSILON 0.05
#define DEFAULT_BIAS_MIN (-0.25)
#define DEFAULT_BIAS_MAX 0.25
#define DEFAULT_HISTORY_MAX_RECORDS 1000U
#define DEFAULT_HISTORY_MAX_DAYS 30.0
#define DEFAULT_HISTORY_LAMBDA 0.1
#define DEFAULT_CLEANUP_INTERVAL 100U
#define DEFAULT_INACTIVE_DAYS 7.0
#define DEFAULT_EXPIRE_DAYS 30.0

static void usage(const char *program)
{
    printf("Usage: %s [options]\n\n", program);
    printf("  --input FILE                 Phase 4/classifier decision input\n");
    printf("  --output FILE                Decision results CSV (default: %s)\n", DEFAULT_OUTPUT);
    printf("  --app-id ID                  Application ID when input has no app_id column\n");
    printf("  --state-dir DIR              Current state directory (default: %s)\n", DEFAULT_STATE_DIR);
    printf("  --history-dir DIR            Bounded history directory (default: %s)\n", DEFAULT_HISTORY_DIR);
    printf("  --log FILE                   Decision log (default: %s)\n", DEFAULT_LOG);
    printf("  --epsilon VALUE              Decision margin (default: %.3f)\n", DEFAULT_EPSILON);
    printf("  --bias-min VALUE             Minimum bounded bias (default: %.3f)\n", DEFAULT_BIAS_MIN);
    printf("  --bias-max VALUE             Maximum bounded bias (default: %.3f)\n", DEFAULT_BIAS_MAX);
    printf("  --memory-bias VALUE          Explicit memory bias override\n");
    printf("  --thread-bias VALUE          Explicit thread bias override\n");
    printf("  --weight A,F,V               Override action/factor weight, e.g. MEMORY,GAIN,2\n");
    printf("  --history-max-records N      Maximum records per history file (default: %u)\n", DEFAULT_HISTORY_MAX_RECORDS);
    printf("  --history-max-days N         Maximum history age (default: %.1f)\n", DEFAULT_HISTORY_MAX_DAYS);
    printf("  --history-lambda VALUE       Historical relevance decay parameter (default: %.3f)\n", DEFAULT_HISTORY_LAMBDA);
    printf("  --cleanup-interval N         Decisions between cleanup attempts (default: %u)\n", DEFAULT_CLEANUP_INTERVAL);
    printf("  --inactive-days N            Mark unseen applications INACTIVE (default: %.1f)\n", DEFAULT_INACTIVE_DAYS);
    printf("  --expire-days N              Mark older unseen applications EXPIRED (default: %.1f)\n", DEFAULT_EXPIRE_DAYS);
    printf("  --help                       Show this help\n");
}

static int parse_size(const char *text, size_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 || parsed > SIZE_MAX)
        return -1;
    *value = (size_t)parsed;
    return 0;
}

static int parse_double(const char *text, double *value, bool positive)
{
    char *end = NULL;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed) || (positive ? parsed <= 0.0 : parsed < 0.0))
        return -1;
    *value = parsed;
    return 0;
}

static int parse_weight_spec(const char *text, decision_weight_override_t *override)
{
    char copy[128];
    char *action;
    char *factor;
    char *value;
    char *save = NULL;
    char *end = NULL;

    if (strlen(text) >= sizeof(copy))
        return -1;
    strcpy(copy, text);
    action = strtok_r(copy, ",", &save);
    factor = strtok_r(NULL, ",", &save);
    value = strtok_r(NULL, ",", &save);
    if (action == NULL || factor == NULL || value == NULL || strtok_r(NULL, ",", &save) != NULL)
        return -1;
    if (strcasecmp(action, "MEMORY") == 0)
        override->action = DECISION_MEMORY;
    else if (strcasecmp(action, "THREAD") == 0)
        override->action = DECISION_THREAD;
    else
        return -1;
    if (strcasecmp(factor, "ACCESS") == 0)
        override->factor = FACTOR_ACCESS;
    else if (strcasecmp(factor, "THRESHOLD") == 0)
        override->factor = FACTOR_THRESHOLD;
    else if (strcasecmp(factor, "GAIN") == 0)
        override->factor = FACTOR_GAIN;
    else if (strcasecmp(factor, "COST") == 0)
        override->factor = FACTOR_COST;
    else if (strcasecmp(factor, "CPU") == 0)
        override->factor = FACTOR_CPU;
    else if (strcasecmp(factor, "SHARING") == 0)
        override->factor = FACTOR_SHARING;
    else
        return -1;
    errno = 0;
    override->value = strtod(value, &end);
    return errno == 0 && end != value && *end == '\0' && isfinite(override->value) && override->value >= 0.0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"input", required_argument, NULL, 'i'},
        {"output", required_argument, NULL, 'o'},
        {"app-id", required_argument, NULL, 'a'},
        {"state-dir", required_argument, NULL, 's'},
        {"history-dir", required_argument, NULL, 'H'},
        {"log", required_argument, NULL, 'l'},
        {"epsilon", required_argument, NULL, 'e'},
        {"bias-min", required_argument, NULL, 'n'},
        {"bias-max", required_argument, NULL, 'x'},
        {"memory-bias", required_argument, NULL, 'm'},
        {"thread-bias", required_argument, NULL, 't'},
        {"weight", required_argument, NULL, 'w'},
        {"history-max-records", required_argument, NULL, 'r'},
        {"history-max-days", required_argument, NULL, 'd'},
        {"history-lambda", required_argument, NULL, 'y'},
        {"cleanup-interval", required_argument, NULL, 'c'},
        {"inactive-days", required_argument, NULL, 'I'},
        {"expire-days", required_argument, NULL, 'X'},
        {"help", no_argument, NULL, '?'},
        {NULL, 0, NULL, 0}
    };
    decision_config_t config = {
        .input_path = DEFAULT_INPUT,
        .output_path = DEFAULT_OUTPUT,
        .state_dir = DEFAULT_STATE_DIR,
        .history_dir = DEFAULT_HISTORY_DIR,
        .log_path = DEFAULT_LOG,
        .app_id = NULL,
        .epsilon = DEFAULT_EPSILON,
        .bias_min = DEFAULT_BIAS_MIN,
        .bias_max = DEFAULT_BIAS_MAX,
        .default_memory_bias = 0.0,
        .default_thread_bias = 0.0,
        .override_bias = false,
        .weight_override_count = 0,
        .history_max_records = DEFAULT_HISTORY_MAX_RECORDS,
        .history_max_days = DEFAULT_HISTORY_MAX_DAYS,
        .history_decay_lambda = DEFAULT_HISTORY_LAMBDA,
        .cleanup_interval = DEFAULT_CLEANUP_INTERVAL,
        .inactive_days = DEFAULT_INACTIVE_DAYS,
        .expire_days = DEFAULT_EXPIRE_DAYS
    };
    decision_summary_t summary;
    int option;
    int result;

    while ((option = getopt_long(argc, argv, "i:o:a:s:H:l:e:n:x:m:t:w:r:d:y:c:I:X:?", options, NULL)) != -1) {
        switch (option) {
        case 'i': config.input_path = optarg; break;
        case 'o': config.output_path = optarg; break;
        case 'a': config.app_id = optarg; break;
        case 's': config.state_dir = optarg; break;
        case 'H': config.history_dir = optarg; break;
        case 'l': config.log_path = optarg; break;
        case 'e':
            if (parse_double(optarg, &config.epsilon, false) != 0) goto invalid;
            break;
        case 'n':
            if (parse_double(optarg, &config.bias_min, false) != 0) goto invalid;
            break;
        case 'x':
            if (parse_double(optarg, &config.bias_max, false) != 0) goto invalid;
            break;
        case 'm':
            if (parse_double(optarg, &config.default_memory_bias, false) != 0) goto invalid;
            config.override_bias = true;
            break;
        case 't':
            if (parse_double(optarg, &config.default_thread_bias, false) != 0) goto invalid;
            config.override_bias = true;
            break;
        case 'w':
            if (config.weight_override_count >= DECISION_MAX_WEIGHT_OVERRIDES ||
                parse_weight_spec(optarg, &config.weight_overrides[config.weight_override_count]) != 0)
                goto invalid;
            config.weight_override_count++;
            break;
        case 'r':
            if (parse_size(optarg, &config.history_max_records) != 0) goto invalid;
            break;
        case 'd':
            if (parse_double(optarg, &config.history_max_days, true) != 0) goto invalid;
            break;
        case 'y':
            if (parse_double(optarg, &config.history_decay_lambda, true) != 0) goto invalid;
            break;
        case 'c':
            if (parse_size(optarg, &config.cleanup_interval) != 0) goto invalid;
            break;
        case 'I':
            if (parse_double(optarg, &config.inactive_days, true) != 0) goto invalid;
            break;
        case 'X':
            if (parse_double(optarg, &config.expire_days, true) != 0) goto invalid;
            break;
        case '?': usage(argv[0]); return EXIT_SUCCESS;
        default: goto invalid;
        }
    }
    if (optind != argc || config.bias_min > config.bias_max || config.expire_days < config.inactive_days)
        goto invalid;
    if (config.override_bias && (config.default_memory_bias < config.bias_min || config.default_memory_bias > config.bias_max ||
                                  config.default_thread_bias < config.bias_min || config.default_thread_bias > config.bias_max))
        goto invalid;
    printf("AWAVMA Decision Engine\n");
    printf("---------------------\n");
    printf("Input             : %s\n", config.input_path);
    printf("Output            : %s\n", config.output_path);
    printf("State directory   : %s\n", config.state_dir);
    printf("History directory : %s\n", config.history_dir);
    printf("Epsilon           : %.6f\n", config.epsilon);
    printf("Bias bounds       : [%.6f, %.6f]\n", config.bias_min, config.bias_max);
    printf("History records   : %zu\n\n", config.history_max_records);
    monitor_profile_scope_t profile;
    monitor_profile_scope_begin(&profile, "phase46_child", "phase5_child_api_total", config.app_id, -1, 0);
    result = decision_run(&config, &summary);
    monitor_profile_scope_end(&profile, result == 0 ? "OK" : "ERROR");
    if (result != 0)
        return EXIT_FAILURE;
    printf("Decision results\n");
    printf("MOVE_MEMORY       : %zu\n", summary.move_memory);
    printf("MOVE_THREAD       : %zu\n", summary.move_thread);
    printf("NO_MIGRATION      : %zu\n", summary.no_migration);
    printf("INSUFFICIENT      : %zu\n", summary.insufficient_rows);
    printf("Applications      : %zu\n", summary.applications);
    printf("Runtime           : %.6f sec\n", summary.runtime_seconds);
    printf("Decision engine completed. No migration executed.\n");
    return EXIT_SUCCESS;

invalid:
    fprintf(stderr, "Error: invalid decision-engine option or value.\n");
    usage(argv[0]);
    return EXIT_FAILURE;
}
