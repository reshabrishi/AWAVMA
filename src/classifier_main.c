#include "classifier.h"
#include "monitor_profile.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_ACCESS_COLUMN "access_value"
#define DEFAULT_ENTITY_COLUMN "entity_id"
#define DEFAULT_WINDOW 10U
#define DEFAULT_LAMBDA 0.1
#define DEFAULT_HOT_THRESHOLD 100.0
#define DEFAULT_MODERATE_THRESHOLD 20.0
#define DEFAULT_HYSTERESIS 5.0
#define DEFAULT_INPUT "results/monitoring_results.csv"
#define DEFAULT_OUTPUT "results/classification_results.csv"

static void usage(const char *program)
{
    printf("Usage: %s [options]\n\n", program);
    printf("  --input FILE              Monitoring/workload CSV (default: %s)\n", DEFAULT_INPUT);
    printf("  --output FILE             Classification CSV (default: %s)\n", DEFAULT_OUTPUT);
    printf("  --access-column NAME      Explicit access observation column (default: %s)\n", DEFAULT_ACCESS_COLUMN);
    printf("  --entity-column NAME      Entity column (default: %s; workload if absent)\n", DEFAULT_ENTITY_COLUMN);
    printf("  --window N                Rolling observation window (default: %u)\n", DEFAULT_WINDOW);
    printf("  --lambda VALUE            Decay factor per second (default: %.3f)\n", DEFAULT_LAMBDA);
    printf("  --hot-threshold VALUE     HOT boundary (default: %.3f)\n", DEFAULT_HOT_THRESHOLD);
    printf("  --moderate-threshold V    MODERATE boundary (default: %.3f)\n", DEFAULT_MODERATE_THRESHOLD);
    printf("  --hysteresis VALUE        Boundary margin (default: %.3f)\n", DEFAULT_HYSTERESIS);
    printf("  --require-access          Fail if access observations are unavailable\n");
    printf("  --help                    Show this help\n");
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

static int parse_positive_double(const char *text, double *value)
{
    char *end = NULL;
    double parsed;

    if (text == NULL || *text == '\0')
        return -1;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed) || parsed <= 0.0)
        return -1;
    *value = parsed;
    return 0;
}

static int parse_nonnegative_double(const char *text, double *value)
{
    char *end = NULL;
    double parsed;

    if (text == NULL || *text == '\0')
        return -1;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed) || parsed < 0.0)
        return -1;
    *value = parsed;
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"input", required_argument, NULL, 'i'},
        {"output", required_argument, NULL, 'o'},
        {"access-column", required_argument, NULL, 'a'},
        {"entity-column", required_argument, NULL, 'e'},
        {"window", required_argument, NULL, 'w'},
        {"lambda", required_argument, NULL, 'l'},
        {"hot-threshold", required_argument, NULL, 'h'},
        {"moderate-threshold", required_argument, NULL, 'm'},
        {"hysteresis", required_argument, NULL, 'y'},
        {"require-access", no_argument, NULL, 'r'},
        {"help", no_argument, NULL, '?'}
    };
    classifier_config_t config;
    classifier_summary_t summary;
    size_t parsed_size;
    int option;
    int result;

    classifier_config_default(&config);
    while ((option = getopt_long(argc, argv, "i:o:a:e:w:l:h:m:y:r?", options, NULL)) != -1) {
        switch (option) {
        case 'i':
            config.input_path = optarg;
            break;
        case 'o':
            config.output_path = optarg;
            break;
        case 'a':
            config.access_column = optarg;
            break;
        case 'e':
            config.entity_column = optarg;
            break;
        case 'w':
            if (parse_size(optarg, &parsed_size) != 0) {
                fprintf(stderr, "Error: invalid rolling window.\n");
                return EXIT_FAILURE;
            }
            config.window_size = parsed_size;
            break;
        case 'l':
            if (parse_positive_double(optarg, &config.lambda) != 0) {
                fprintf(stderr, "Error: lambda must be a positive finite number.\n");
                return EXIT_FAILURE;
            }
            break;
        case 'h':
            if (parse_nonnegative_double(optarg, &config.hot_threshold) != 0) {
                fprintf(stderr, "Error: invalid HOT threshold.\n");
                return EXIT_FAILURE;
            }
            break;
        case 'm':
            if (parse_nonnegative_double(optarg, &config.moderate_threshold) != 0) {
                fprintf(stderr, "Error: invalid MODERATE threshold.\n");
                return EXIT_FAILURE;
            }
            break;
        case 'y':
            if (parse_nonnegative_double(optarg, &config.hysteresis) != 0) {
                fprintf(stderr, "Error: invalid hysteresis value.\n");
                return EXIT_FAILURE;
            }
            break;
        case 'r':
            config.require_access = true;
            break;
        case '?':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind != argc) {
        fprintf(stderr, "Error: unexpected argument '%s'.\n", argv[optind]);
        return EXIT_FAILURE;
    }
    if (config.hot_threshold <= config.moderate_threshold) {
        fprintf(stderr, "Error: HOT threshold must be greater than MODERATE threshold.\n");
        return EXIT_FAILURE;
    }
    if (config.hysteresis > config.moderate_threshold) {
        fprintf(stderr, "Error: hysteresis must not exceed the MODERATE threshold.\n");
        return EXIT_FAILURE;
    }
    printf("AWAVMA Workload Classifier\n");
    printf("--------------------------\n");
    printf("Input              : %s\n", config.input_path);
    printf("Output             : %s\n", config.output_path);
    printf("Access column      : %s\n", config.access_column);
    printf("Window             : %zu observations\n", config.window_size);
    printf("Decay factor       : %.6f\n", config.lambda);
    printf("Hot threshold      : %.6f\n", config.hot_threshold);
    printf("Moderate threshold : %.6f\n", config.moderate_threshold);
    printf("Hysteresis         : %.6f\n\n", config.hysteresis);
    monitor_profile_scope_t profile;
    monitor_profile_scope_begin(&profile, "phase46_child", "phase4_child_api_total", NULL, -1, 0);
    result = classifier_run(&config, &summary);
    monitor_profile_scope_end(&profile, result == 0 ? "OK" : "ERROR");
    if (result < 0)
        return result == -2 ? 2 : EXIT_FAILURE;
    printf("Classification results\n");
    printf("HOT       : %zu rows\n", summary.hot_rows);
    printf("MODERATE  : %zu rows\n", summary.moderate_rows);
    printf("COLD      : %zu rows\n", summary.cold_rows);
    printf("UNAVAILABLE: %zu rows\n", summary.unavailable_rows);
    printf("Rows      : %zu\n", summary.rows);
    printf("Runtime   : %.6f sec\n", summary.elapsed_seconds);
    printf("Classification completed.\n");
    return EXIT_SUCCESS;
}
