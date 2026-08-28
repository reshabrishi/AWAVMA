#include "awavma_runtime.h"
#include "runtime_target_filter.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int parse_u64(const char *text, uint64_t *value, bool allow_zero)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || (!allow_zero && parsed == 0))
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n\n", program);
    printf("  --duration-ms N       Run length; 0 runs until SIGINT/SIGTERM (default: 1000)\n");
    printf("  --evaluation-ms N     Stable pipeline evaluation interval (default: 1000)\n");
    printf("  --monitor-ms N        Per-application Phase 3 interval (default: 100)\n");
    printf("  --discovery-interval-ms N  /proc refresh interval; required positive value\n");
    printf("  --workers N           Phase 3 worker count (default: 2)\n");
    printf("  --queue-capacity N    Phase 3 FIFO queue capacity (default: 64)\n");
    printf("  --root-dir DIR        Runtime artifact root (default: results/runtime)\n");
    printf("  --bin-dir DIR         Existing phase binary directory (default: bin)\n");
    printf("  --config FILE         Phase 6 configuration file (default: config/awavma.conf)\n");
    printf("  --phase4-mode MODE    subprocess or in-process (default: subprocess)\n");
    printf("  --pid PID             Monitor an existing application PID (repeatable)\n");
    printf("  --help                Show this help\n");
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"duration-ms", required_argument, NULL, 'd'},
        {"evaluation-ms", required_argument, NULL, 'e'},
        {"monitor-ms", required_argument, NULL, 'm'},
        {"discovery-interval-ms", required_argument, NULL, 'D'},
        {"workers", required_argument, NULL, 'w'},
        {"queue-capacity", required_argument, NULL, 'q'},
        {"root-dir", required_argument, NULL, 'r'},
        {"bin-dir", required_argument, NULL, 'b'},
        {"config", required_argument, NULL, 'c'},
        {"phase4-mode", required_argument, NULL, 'P'},
        {"pid", required_argument, NULL, 'p'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    uint64_t duration_ms = 1000;
    uint64_t value;
    runtime_target_filter_t target_filter;
    int option;
    int result = EXIT_FAILURE;

    awavma_runtime_config_default(&config);
    runtime_target_filter_init(&target_filter);
    while ((option = getopt_long(argc, argv, "d:e:m:D:w:q:r:b:c:P:p:h", options, NULL)) != -1) {
        switch (option) {
        case 'd':
            if (parse_u64(optarg, &duration_ms, true) != 0) goto invalid;
            break;
        case 'e':
            if (parse_u64(optarg, &value, false) != 0 || value > SIZE_MAX) goto invalid;
            config.evaluation_interval_ms = value;
            break;
        case 'm':
            if (parse_u64(optarg, &value, false) != 0) goto invalid;
            config.monitor_interval_ms = value;
            break;
        case 'D':
            if (parse_u64(optarg, &value, false) != 0) goto invalid;
            config.discovery_interval_ms = value;
            break;
        case 'w':
            if (parse_u64(optarg, &value, false) != 0 || value > SIZE_MAX) goto invalid;
            config.worker_count = (size_t)value;
            break;
        case 'q':
            if (parse_u64(optarg, &value, false) != 0 || value > SIZE_MAX) goto invalid;
            config.queue_capacity = (size_t)value;
            break;
        case 'r': config.root_dir = optarg; break;
        case 'b': config.bin_dir = optarg; break;
        case 'c': config.phase_config_path = optarg; break;
        case 'P':
            if (strcmp(optarg, "subprocess") == 0)
                config.phase4_mode = AWAVMA_PHASE4_SUBPROCESS;
            else if (strcmp(optarg, "in-process") == 0)
                config.phase4_mode = AWAVMA_PHASE4_IN_PROCESS;
            else
                goto invalid;
            break;
        case 'p':
            if (parse_u64(optarg, &value, false) != 0 || value > (uint64_t)INT_MAX)
                goto invalid;
            if (runtime_target_filter_add_pid(&target_filter, (pid_t)value) != 0) {
                fprintf(stderr, "Error: target PID %llu is unavailable.\n",
                        (unsigned long long)value);
                goto failed;
            }
            break;
        case 'h':
            usage(argv[0]);
            runtime_target_filter_cleanup(&target_filter);
            return EXIT_SUCCESS;
        default: goto invalid;
        }
    }
    if (optind != argc)
        goto invalid;
    if (target_filter.count > 0) {
        config.application_filter = runtime_target_filter_matches;
        config.application_filter_context = &target_filter;
    }
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    runtime = awavma_runtime_create();
    if (runtime == NULL || awavma_runtime_init(runtime, &config) != 0) {
        fprintf(stderr, "Error: unable to initialize AWAVMA runtime.\n");
        awavma_runtime_destroy(runtime);
        goto failed;
    }
    result = EXIT_SUCCESS;
    do {
        uint64_t slice = config.evaluation_interval_ms;

        if (duration_ms != 0 && duration_ms < slice)
            slice = duration_ms;
        if (awavma_runtime_run_for(runtime, slice) != 0) {
            result = EXIT_FAILURE;
            break;
        }
        if (duration_ms != 0)
            duration_ms -= slice;
    } while (!stop_requested && duration_ms != 0);
    if (stop_requested) {
        awavma_runtime_request_stop(runtime);
        result = EXIT_SUCCESS;
    }
    awavma_runtime_destroy(runtime);
    runtime_target_filter_cleanup(&target_filter);
    return result;

invalid:
    fprintf(stderr, "Error: invalid runtime option.\n");
    usage(argv[0]);
failed:
    runtime_target_filter_cleanup(&target_filter);
    return EXIT_FAILURE;
}
