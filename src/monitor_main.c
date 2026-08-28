#include "monitor.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_INTERVAL_MS 100U
#define DEFAULT_OUTPUT "results/monitoring_results.csv"
#define DEFAULT_THREAD_OUTPUT "results/monitoring_threads.csv"
#define DEFAULT_LOG "logs/monitor.log"
#define DEFAULT_BENCHMARK "bin/benchmark"

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void usage(const char *program)
{
    printf("Usage:\n");
    printf("  %s --pid PID [monitor options]\n", program);
    printf("  %s [monitor options] --benchmark [benchmark options]\n\n", program);
    printf("Monitor options:\n");
    printf("  --pid PID                 Monitor an existing process\n");
    printf("  --benchmark               Launch bin/benchmark with remaining arguments\n");
    printf("  --benchmark-path PATH     Benchmark executable (default: %s)\n", DEFAULT_BENCHMARK);
    printf("  --interval MS             Sampling interval (default: %u)\n", DEFAULT_INTERVAL_MS);
    printf("  --output FILE             Process monitoring CSV (default: %s)\n", DEFAULT_OUTPUT);
    printf("  --thread-output FILE      Per-thread monitoring CSV (default: %s)\n", DEFAULT_THREAD_OUTPUT);
    printf("  --log FILE                Monitor log (default: %s)\n", DEFAULT_LOG);
    printf("  --help                    Show this help\n\n");
    printf("For launcher mode, place --benchmark before all benchmark arguments.\n");
}

static int parse_positive_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0)
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static const char *long_option_value(int argc, char **argv, int *index, const char *name)
{
    size_t name_length = strlen(name);
    const char *argument = argv[*index];

    if (strncmp(argument, name, name_length) == 0 && argument[name_length] == '=')
        return argument + name_length + 1;
    if (strcmp(argument, name) == 0 && *index + 1 < argc)
        return argv[++*index];
    return NULL;
}

static const char *short_option_value(int argc, char **argv, int *index, char option)
{
    const char *argument = argv[*index];

    if (argument[0] == '-' && argument[1] == option && argument[2] != '\0')
        return argument + 2;
    if (strcmp(argument, option == 't' ? "-t" : option == 'm' ? "-m" : "-i") == 0 &&
        *index + 1 < argc)
        return argv[++*index];
    return NULL;
}

static void collect_metadata(int argc, char **argv, monitor_metadata_t *metadata)
{
    int index;

    memset(metadata, 0, sizeof(*metadata));
    for (index = 0; index < argc; index++) {
        const char *value;

        value = long_option_value(argc, argv, &index, "--pattern");
        if (value != NULL) {
            metadata->pattern = value;
            continue;
        }
        value = long_option_value(argc, argv, &index, "--threads");
        if (value != NULL) {
            metadata->threads = value;
            continue;
        }
        value = long_option_value(argc, argv, &index, "--memory");
        if (value != NULL) {
            metadata->memory_mb = value;
            continue;
        }
        value = long_option_value(argc, argv, &index, "--iterations");
        if (value != NULL) {
            metadata->iterations = value;
            continue;
        }
        value = long_option_value(argc, argv, &index, "--duration");
        if (value != NULL) {
            metadata->duration = value;
            continue;
        }
        value = short_option_value(argc, argv, &index, 't');
        if (value != NULL) {
            metadata->threads = value;
            continue;
        }
        value = short_option_value(argc, argv, &index, 'm');
        if (value != NULL) {
            metadata->memory_mb = value;
            continue;
        }
        value = short_option_value(argc, argv, &index, 'i');
        if (value != NULL)
            metadata->iterations = value;
    }
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) == 0 &&
                   sigaction(SIGTERM, &action, NULL) == 0 ? 0 : -1;
}

static int wait_for_child(pid_t child, int *status)
{
    while (waitpid(child, status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"pid", required_argument, NULL, 'p'},
        {"interval", required_argument, NULL, 'i'},
        {"output", required_argument, NULL, 'o'},
        {"thread-output", required_argument, NULL, 't'},
        {"log", required_argument, NULL, 'l'},
        {"benchmark-path", required_argument, NULL, 'b'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    monitor_config_t config = {
        .pid = -1,
        .interval_ms = DEFAULT_INTERVAL_MS,
        .output_path = DEFAULT_OUTPUT,
        .thread_output_path = DEFAULT_THREAD_OUTPUT,
        .log_path = DEFAULT_LOG,
        .target_is_child = false,
        .child_status = NULL,
        .stop_requested = &stop_requested
    };
    const char *benchmark_path = DEFAULT_BENCHMARK;
    char **benchmark_argv = NULL;
    int benchmark_argc = 0;
    int benchmark_marker = -1;
    int option;
    int index;
    int child_status = -1;
    pid_t child = -1;
    bool launcher = false;
    uint64_t parsed;
    int monitor_result;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--benchmark") == 0) {
            benchmark_marker = index;
            launcher = true;
            break;
        }
    }
    optind = 1;
    while ((option = getopt_long(benchmark_marker >= 0 ? benchmark_marker : argc, argv,
                                 "p:i:o:t:l:b:h", options, NULL)) != -1) {
        switch (option) {
        case 'p':
            if (parse_positive_u64(optarg, &parsed) != 0 || parsed > (uint64_t)INT32_MAX) {
                fprintf(stderr, "Error: invalid PID.\n");
                return EXIT_FAILURE;
            }
            config.pid = (pid_t)parsed;
            break;
        case 'i':
            if (parse_positive_u64(optarg, &parsed) != 0 || parsed > UINT32_MAX) {
                fprintf(stderr, "Error: invalid monitoring interval.\n");
                return EXIT_FAILURE;
            }
            config.interval_ms = (unsigned)parsed;
            break;
        case 'o':
            config.output_path = optarg;
            break;
        case 't':
            config.thread_output_path = optarg;
            break;
        case 'l':
            config.log_path = optarg;
            break;
        case 'b':
            benchmark_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (benchmark_marker >= 0) {
        benchmark_argc = argc - benchmark_marker - 1;
        benchmark_argv = &argv[benchmark_marker + 1];
        if (benchmark_argc == 0) {
            fprintf(stderr, "Error: --benchmark requires benchmark arguments.\n");
            return EXIT_FAILURE;
        }
        if (config.pid > 0) {
            fprintf(stderr, "Error: choose either --pid or --benchmark, not both.\n");
            return EXIT_FAILURE;
        }
    } else if (config.pid <= 0) {
        fprintf(stderr, "Error: specify --pid PID or --benchmark.\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (install_signal_handlers() != 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    if (launcher) {
        char **child_argv = calloc((size_t)benchmark_argc + 2, sizeof(*child_argv));

        if (child_argv == NULL) {
            perror("calloc");
            return EXIT_FAILURE;
        }
        child_argv[0] = (char *)benchmark_path;
        for (index = 0; index < benchmark_argc; index++)
            child_argv[index + 1] = benchmark_argv[index];
        child = fork();
        if (child < 0) {
            perror("fork");
            free(child_argv);
            return EXIT_FAILURE;
        }
        if (child == 0) {
            execv(benchmark_path, child_argv);
            perror("execv benchmark");
            _exit(127);
        }
        free(child_argv);
        config.pid = child;
        config.target_is_child = true;
        config.child_status = &child_status;
        collect_metadata(benchmark_argc, benchmark_argv, &config.metadata);
    }
    monitor_result = monitor_run_pid(&config);
    if (launcher && child_status < 0) {
        if (monitor_result != 0 || stop_requested)
            kill(child, SIGTERM);
        if (wait_for_child(child, &child_status) != 0)
            monitor_result = -1;
    }
    if (launcher) {
        if (WIFEXITED(child_status))
            printf("Benchmark exit status: %d\n", WEXITSTATUS(child_status));
        else if (WIFSIGNALED(child_status))
            printf("Benchmark terminated by signal: %d\n", WTERMSIG(child_status));
        if (monitor_result == 0 && WIFEXITED(child_status) && WEXITSTATUS(child_status) != 0)
            monitor_result = WEXITSTATUS(child_status);
    }
    return monitor_result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
