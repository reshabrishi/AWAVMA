#define _POSIX_C_SOURCE 200809L

#include "application_discovery.h"
#include "application_manager.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(const char *program)
{
    printf("Usage: %s [--idle-timeout-ms N]\n", program);
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"idle-timeout-ms", required_argument, NULL, 'i'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    application_manager_config_t manager_config;
    application_discovery_config_t discovery_config;
    application_manager_t *manager = NULL;
    application_discovery_t *discovery = NULL;
    application_discovery_record_t *discovered = NULL;
    application_manager_record_t *active = NULL;
    size_t discovered_count;
    size_t active_count;
    int option;
    int result = EXIT_FAILURE;

    application_manager_config_default(&manager_config);
    while ((option = getopt_long(argc, argv, "i:h", options, NULL)) != -1) {
        uint64_t value;

        switch (option) {
        case 'i':
            if (parse_u64(optarg, &value) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            manager_config.application_idle_timeout_ms = value;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind != argc)
        return EXIT_FAILURE;

    manager = application_manager_create();
    discovery = application_discovery_create();
    if (manager == NULL || discovery == NULL ||
        application_manager_init(manager, &manager_config) != 0) {
        fprintf(stderr, "application manager initialization failed\n");
        goto cleanup;
    }
    application_discovery_config_default(&discovery_config);
    if (application_discovery_init(discovery, &discovery_config) != 0 ||
        application_discovery_scan(discovery) != 0)
        goto cleanup;
    discovered_count = application_discovery_count(discovery);
    discovered = calloc(discovered_count == 0 ? 1 : discovered_count, sizeof(*discovered));
    if (discovered == NULL)
        goto cleanup;
    for (size_t index = 0; index < discovered_count; index++)
        if (!application_discovery_get(discovery, index, &discovered[index]))
            goto cleanup;
    if (application_manager_process_snapshot(manager, discovered, discovered_count) != 0)
        goto cleanup;

    active_count = application_manager_active_count(manager);
    active = calloc(active_count == 0 ? 1 : active_count, sizeof(*active));
    if (active == NULL)
        goto cleanup;
    active_count = application_manager_snapshot(manager, active, active_count);
    printf("APP_ID | PID | NAME | START_TIME | LAST_SEEN | STATUS\n");
    for (size_t index = 0; index < active_count; index++)
        printf("%s | %ld | %s | %llu | %lld | %s\n", active[index].app_id,
               (long)active[index].pid, active[index].process_name,
               (unsigned long long)active[index].start_time_ticks,
               (long long)active[index].last_seen,
               application_manager_status_name(active[index].status));
    printf("Active applications: %zu\n", active_count);
    result = EXIT_SUCCESS;

cleanup:
    free(active);
    free(discovered);
    application_manager_destroy(manager);
    application_discovery_destroy(discovery);
    return result;
}
