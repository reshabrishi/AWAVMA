#define _POSIX_C_SOURCE 200809L

#include "application_discovery.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double monotonic_seconds(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void usage(const char *program)
{
    printf("Usage: %s [--include-pid1] [--include-self]\n", program);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"include-pid1", no_argument, NULL, '1'},
        {"include-self", no_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    application_discovery_config_t config;
    application_discovery_t *discovery;
    int option;
    double start;

    application_discovery_config_default(&config);
    while ((option = getopt_long(argc, argv, "1sh", options, NULL)) != -1) {
        switch (option) {
        case '1': config.include_pid1 = true; break;
        case 's': config.excluded_pid = 0; break;
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    discovery = application_discovery_create();
    if (discovery == NULL || application_discovery_init(discovery, &config) != 0) {
        application_discovery_destroy(discovery);
        fprintf(stderr, "application discovery initialization failed\n");
        return EXIT_FAILURE;
    }
    start = monotonic_seconds();
    if (application_discovery_scan(discovery) != 0) {
        application_discovery_destroy(discovery);
        fprintf(stderr, "application discovery scan failed\n");
        return EXIT_FAILURE;
    }
    printf("PID\tNAME\tPARENT PID\tSTART TIME\tSTATE\tEXECUTABLE\n");
    for (size_t index = 0; index < application_discovery_count(discovery); index++) {
        application_discovery_record_t record;

        if (!application_discovery_get(discovery, index, &record))
            continue;
        printf("%ld\t%s\t%ld\t%llu\t%c\t%s\n", (long)record.pid,
               record.process_name, (long)record.parent_pid,
               (unsigned long long)record.start_time_ticks, record.state,
               record.executable_path[0] == '\0' ? "-" : record.executable_path);
    }
    printf("Discovered: %zu processes\n", application_discovery_count(discovery));
    printf("Scan duration: %.6f sec\n", monotonic_seconds() - start);
    application_discovery_destroy(discovery);
    return EXIT_SUCCESS;
}
