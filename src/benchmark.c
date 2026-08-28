#include "benchmark.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <numa.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_THREADS 1U
#define DEFAULT_MEMORY_MB 64U
#define DEFAULT_ITERATIONS 1000000ULL
#define DEFAULT_HOT_PERCENT 10U
#define DEFAULT_MODERATE_PERCENT 30U
#define DEFAULT_COLD_PERCENT 60U
#define DEFAULT_CHANGE_PHASES 3U
#define DEFAULT_SEED 12345ULL
#define DURATION_CHECK_BATCH 1024ULL

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t ready;
    bool start;
    bool abort;
} start_gate_t;

typedef struct {
    uint64_t operations;
    double elapsed_sec;
    uint64_t checksum;
    int cpu;
    int cpu_node;
} worker_stats_t;

typedef struct {
    const benchmark_config_t *config;
    volatile uint64_t *data;
    size_t elements;
    size_t memory_bytes;
    size_t thread_id;
    start_gate_t *gate;
    struct timespec start_time;
    atomic_int *worker_error;
    worker_stats_t *stats;
} worker_context_t;

static const char *pattern_names[] = {
    "sequential", "random", "hot", "moderate", "cold", "mixed",
    "local", "remote", "changing"
};

const char *benchmark_pattern_name(benchmark_pattern_t pattern)
{
    if (pattern < PATTERN_SEQUENTIAL || pattern > PATTERN_CHANGING)
        return "unknown";
    return pattern_names[pattern];
}

static void print_usage(const char *program)
{
    printf("Usage: %s [options]\n\n", program);
    printf("Required workload options have defaults and can be overridden:\n");
    printf("  -t, --threads N              Worker thread count (default: %u)\n", DEFAULT_THREADS);
    printf("  -m, --memory MB              Allocated memory in MiB (default: %u)\n", DEFAULT_MEMORY_MB);
    printf("  -i, --iterations N           Operations per thread (default: %llu)\n",
           (unsigned long long)DEFAULT_ITERATIONS);
    printf("  -d, --duration SEC           Duration mode; takes precedence over iterations\n");
    printf("  -p, --pattern NAME           sequential, random, hot, moderate, cold,\n");
    printf("                               mixed, local, remote, or changing\n");
    printf("      --hot-percent N          Hot region percentage (default: %u)\n", DEFAULT_HOT_PERCENT);
    printf("      --moderate-percent N     Moderate region percentage (default: %u)\n", DEFAULT_MODERATE_PERCENT);
    printf("      --cold-percent N         Cold region percentage (default: %u)\n", DEFAULT_COLD_PERCENT);
    printf("      --change-phases N        Changing phases (default: %u)\n", DEFAULT_CHANGE_PHASES);
    printf("  -s, --seed N                 Deterministic random seed (default: %llu)\n",
           (unsigned long long)DEFAULT_SEED);
    printf("  -T, --thread-node N          Bind workers to NUMA node N\n");
    printf("  -M, --memory-node N          Allocate memory on NUMA node N\n");
    printf("  -n, --numa-node N            Set both thread and memory node to N\n");
    printf("  -o, --output FILE            Append one result row to CSV FILE\n");
    printf("  -h, --help                   Show this help\n");
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

static int parse_positive_size(const char *text, size_t *value)
{
    uint64_t parsed;

    if (parse_u64(text, &parsed) != 0 || parsed == 0 || parsed > SIZE_MAX)
        return -1;
    *value = (size_t)parsed;
    return 0;
}

static int parse_node(const char *text, int *value)
{
    uint64_t parsed;

    if (parse_u64(text, &parsed) != 0 || parsed > INT32_MAX)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int parse_percent(const char *text, unsigned *value)
{
    uint64_t parsed;

    if (parse_u64(text, &parsed) != 0 || parsed == 0 || parsed > 100)
        return -1;
    *value = (unsigned)parsed;
    return 0;
}

static int parse_duration(const char *text, double *value)
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

static int parse_pattern(const char *text, benchmark_pattern_t *pattern)
{
    benchmark_pattern_t candidate;

    for (candidate = PATTERN_SEQUENTIAL; candidate <= PATTERN_CHANGING; candidate++) {
        if (strcasecmp(text, benchmark_pattern_name(candidate)) == 0) {
            *pattern = candidate;
            return 0;
        }
    }
    return -1;
}

static int parse_options(int argc, char **argv, benchmark_config_t *config)
{
    enum {
        OPTION_HOT_PERCENT = 1000,
        OPTION_MODERATE_PERCENT,
        OPTION_COLD_PERCENT,
        OPTION_CHANGE_PHASES
    };
    static const struct option options[] = {
        {"threads", required_argument, NULL, 't'},
        {"memory", required_argument, NULL, 'm'},
        {"iterations", required_argument, NULL, 'i'},
        {"duration", required_argument, NULL, 'd'},
        {"pattern", required_argument, NULL, 'p'},
        {"numa-node", required_argument, NULL, 'n'},
        {"thread-node", required_argument, NULL, 'T'},
        {"memory-node", required_argument, NULL, 'M'},
        {"hot-percent", required_argument, NULL, OPTION_HOT_PERCENT},
        {"moderate-percent", required_argument, NULL, OPTION_MODERATE_PERCENT},
        {"cold-percent", required_argument, NULL, OPTION_COLD_PERCENT},
        {"change-phases", required_argument, NULL, OPTION_CHANGE_PHASES},
        {"seed", required_argument, NULL, 's'},
        {"output", required_argument, NULL, 'o'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int option;

    while ((option = getopt_long(argc, argv, "t:m:i:d:p:n:T:M:s:o:h", options, NULL)) != -1) {
        size_t size_value;
        uint64_t integer_value;

        switch (option) {
        case 't':
            if (parse_positive_size(optarg, &size_value) != 0)
                goto invalid_argument;
            config->threads = size_value;
            break;
        case 'm':
            if (parse_positive_size(optarg, &size_value) != 0)
                goto invalid_argument;
            config->memory_mb = size_value;
            break;
        case 'i':
            if (parse_u64(optarg, &integer_value) != 0 || integer_value == 0)
                goto invalid_argument;
            config->iterations = integer_value;
            break;
        case 'd':
            if (parse_duration(optarg, &config->duration_sec) != 0)
                goto invalid_argument;
            config->duration_set = true;
            break;
        case 'p':
            if (parse_pattern(optarg, &config->pattern) != 0)
                goto invalid_argument;
            break;
        case 'n':
            if (parse_node(optarg, &config->thread_node) != 0)
                goto invalid_argument;
            config->memory_node = config->thread_node;
            break;
        case 'T':
            if (parse_node(optarg, &config->thread_node) != 0)
                goto invalid_argument;
            break;
        case 'M':
            if (parse_node(optarg, &config->memory_node) != 0)
                goto invalid_argument;
            break;
        case 's':
            if (parse_u64(optarg, &config->seed) != 0)
                goto invalid_argument;
            break;
        case 'o':
            config->output_path = optarg;
            break;
        case OPTION_HOT_PERCENT:
            if (parse_percent(optarg, &config->hot_percent) != 0)
                goto invalid_argument;
            break;
        case OPTION_MODERATE_PERCENT:
            if (parse_percent(optarg, &config->moderate_percent) != 0)
                goto invalid_argument;
            break;
        case OPTION_COLD_PERCENT:
            if (parse_percent(optarg, &config->cold_percent) != 0)
                goto invalid_argument;
            break;
        case OPTION_CHANGE_PHASES:
            if (parse_positive_size(optarg, &size_value) != 0 || size_value > 16)
                goto invalid_argument;
            config->change_phases = (unsigned)size_value;
            break;
        case 'h':
            print_usage(argv[0]);
            return 1;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "Error: unexpected argument '%s'.\n", argv[optind]);
        return -1;
    }
    return 0;

invalid_argument:
    fprintf(stderr, "Error: invalid value for option near '%s'.\n", optarg);
    return -1;
}

static double timespec_seconds(const struct timespec *time)
{
    return (double)time->tv_sec + (double)time->tv_nsec / 1000000000.0;
}

static double elapsed_since(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return timespec_seconds(&now) - timespec_seconds(start);
}

static uint64_t next_random(uint64_t *state)
{
    uint64_t value = *state;

    if (value == 0)
        value = DEFAULT_SEED;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static size_t percent_offset(size_t elements, unsigned percent)
{
    return (elements / 100U) * percent + (elements % 100U) * percent / 100U;
}

static size_t random_in_range(uint64_t *state, size_t begin, size_t end)
{
    size_t span;

    if (end <= begin + 1)
        return begin;
    span = end - begin;
    return begin + (size_t)(next_random(state) % span);
}

static size_t choose_index(const worker_context_t *worker, uint64_t operation,
                           uint64_t *random_state, size_t *changing_phase)
{
    const benchmark_config_t *config = worker->config;
    size_t thread_begin = worker->thread_id * worker->elements / config->threads;
    size_t thread_end = (worker->thread_id + 1) * worker->elements / config->threads;
    size_t hot_end = percent_offset(worker->elements, config->hot_percent);
    size_t moderate_end = hot_end + percent_offset(worker->elements, config->moderate_percent);
    size_t index;

    if (thread_end <= thread_begin)
        thread_end = thread_begin + 1;

    switch (config->pattern) {
    case PATTERN_SEQUENTIAL:
    case PATTERN_LOCAL:
    case PATTERN_REMOTE:
        return thread_begin + (size_t)(operation % (thread_end - thread_begin));
    case PATTERN_RANDOM:
        return random_in_range(random_state, thread_begin, thread_end);
    case PATTERN_HOT:
        if (next_random(random_state) % 100U < 99U)
            return random_in_range(random_state, 0, hot_end);
        return random_in_range(random_state, 0, worker->elements);
    case PATTERN_MODERATE:
        if (next_random(random_state) % 100U < 70U)
            return random_in_range(random_state, hot_end, moderate_end);
        return random_in_range(random_state, 0, worker->elements);
    case PATTERN_COLD:
        if (next_random(random_state) % 100U < 10U)
            return random_in_range(random_state, 0, hot_end);
        return random_in_range(random_state, 0, worker->elements);
    case PATTERN_MIXED: {
        uint64_t choice = next_random(random_state) % 100U;

        if (choice < 70U)
            return random_in_range(random_state, 0, hot_end);
        if (choice < 95U)
            return random_in_range(random_state, hot_end, moderate_end);
        return random_in_range(random_state, moderate_end, worker->elements);
    }
    case PATTERN_CHANGING: {
        size_t phase_size = worker->elements / config->change_phases;
        size_t phase;
        size_t begin;
        size_t end;

        if (config->duration_set) {
            double phase_duration = config->duration_sec / config->change_phases;
            double elapsed = elapsed_since(&worker->start_time);
            phase = (size_t)(elapsed / phase_duration);
        } else {
            uint64_t operations_per_phase = config->iterations / config->change_phases;
            if (operations_per_phase == 0)
                operations_per_phase = 1;
            phase = (size_t)(operation / operations_per_phase);
        }
        if (phase >= config->change_phases)
            phase = config->change_phases - 1;
        *changing_phase = phase;
        begin = phase * phase_size;
        end = phase + 1 == config->change_phases ? worker->elements : begin + phase_size;
        index = random_in_range(random_state, begin, end);
        return index;
    }
    }
    return 0;
}

static int bind_worker_to_node(const benchmark_config_t *config, size_t thread_id,
                               worker_stats_t *stats)
{
    struct bitmask *cpu_mask;
    size_t selected = 0;
    unsigned int cpu_count;
    unsigned int cpu;
    cpu_set_t affinity;
    int result;

    if (config->thread_node < 0)
        return 0;
    cpu_mask = numa_allocate_cpumask();
    if (cpu_mask == NULL)
        return -1;
    if (numa_node_to_cpus(config->thread_node, cpu_mask) != 0) {
        numa_bitmask_free(cpu_mask);
        return -1;
    }
    cpu_count = numa_bitmask_weight(cpu_mask);
    if (cpu_count == 0) {
        numa_bitmask_free(cpu_mask);
        return -1;
    }
    CPU_ZERO(&affinity);
    for (cpu = 0; cpu < cpu_mask->size; cpu++) {
        if (numa_bitmask_isbitset(cpu_mask, cpu)) {
            if (selected++ == thread_id % (size_t)cpu_count) {
                if (cpu >= CPU_SETSIZE) {
                    numa_bitmask_free(cpu_mask);
                    return -1;
                }
                CPU_SET(cpu, &affinity);
                stats->cpu = (int)cpu;
                break;
            }
        }
    }
    numa_bitmask_free(cpu_mask);
    if (CPU_COUNT(&affinity) == 0)
        return -1;
    result = pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
    if (result != 0)
        return result;
    stats->cpu = sched_getcpu();
    stats->cpu_node = numa_node_of_cpu(stats->cpu);
    return 0;
}

static void *worker_main(void *argument)
{
    worker_context_t *worker = argument;
    const benchmark_config_t *config = worker->config;
    worker_stats_t *stats = worker->stats;
    uint64_t random_state = config->seed ^ (0x9e3779b97f4a7c15ULL * (worker->thread_id + 1));
    uint64_t operations = 0;
    uint64_t checksum = 0;
    size_t changing_phase = 0;
    volatile uint64_t *data = worker->data;
    bool abort;
    struct timespec local_start;

    stats->cpu = -1;
    stats->cpu_node = -1;
    if (bind_worker_to_node(config, worker->thread_id, stats) != 0)
        atomic_store(worker->worker_error, 1);

    pthread_mutex_lock(&worker->gate->mutex);
    worker->gate->ready++;
    pthread_cond_broadcast(&worker->gate->condition);
    while (!worker->gate->start)
        pthread_cond_wait(&worker->gate->condition, &worker->gate->mutex);
    abort = worker->gate->abort;
    local_start = worker->start_time;
    pthread_mutex_unlock(&worker->gate->mutex);
    if (abort)
        return NULL;

    while (true) {
        uint64_t batch_end = operations > UINT64_MAX - DURATION_CHECK_BATCH
                                 ? UINT64_MAX
                                 : operations + DURATION_CHECK_BATCH;

        if (!config->duration_set && batch_end > config->iterations)
            batch_end = config->iterations;
        while (operations < batch_end) {
            size_t index = choose_index(worker, operations, &random_state, &changing_phase);
            checksum ^= data[index] + (uint64_t)index;
            operations++;
        }
        if (!config->duration_set) {
            if (operations >= config->iterations)
                break;
        } else if (elapsed_since(&local_start) >= config->duration_sec || operations == UINT64_MAX) {
            break;
        }
    }

    stats->operations = operations;
    stats->checksum = checksum;
    stats->elapsed_sec = elapsed_since(&local_start);
    return NULL;
}

static int first_available_node(void)
{
    int node;
    int max_node = numa_max_node();

    for (node = 0; node <= max_node; node++) {
        if (numa_bitmask_isbitset(numa_all_nodes_ptr, node))
            return node;
    }
    return -1;
}

static int second_available_node(int first)
{
    int node;
    int max_node = numa_max_node();

    for (node = 0; node <= max_node; node++) {
        if (node != first && numa_bitmask_isbitset(numa_all_nodes_ptr, node))
            return node;
    }
    return -1;
}

static int validate_and_resolve_numa(benchmark_config_t *config, int *numa_nodes)
{
    bool placement_requested = config->thread_node >= 0 || config->memory_node >= 0;
    int first;
    int second;

    *numa_nodes = 0;
    if (numa_available() < 0) {
        if (placement_requested || config->pattern == PATTERN_LOCAL || config->pattern == PATTERN_REMOTE) {
            fprintf(stderr, "Error: NUMA placement was requested, but libnuma reports no NUMA support.\n");
            return -1;
        }
        return 0;
    }
    *numa_nodes = numa_num_configured_nodes();
    first = first_available_node();
    if (first < 0) {
        fprintf(stderr, "Error: libnuma found no available NUMA nodes.\n");
        return -1;
    }
    if (config->pattern == PATTERN_LOCAL) {
        if (config->thread_node < 0 && config->memory_node < 0)
            config->thread_node = config->memory_node = first;
        else if (config->thread_node < 0)
            config->thread_node = config->memory_node;
        else if (config->memory_node < 0)
            config->memory_node = config->thread_node;
        if (config->thread_node != config->memory_node) {
            fprintf(stderr, "Error: local pattern requires identical thread and memory nodes.\n");
            return -1;
        }
    } else if (config->pattern == PATTERN_REMOTE) {
        second = second_available_node(first);
        if (*numa_nodes < 2 || second < 0) {
            fprintf(stderr, "Error: remote NUMA testing requires at least 2 available NUMA nodes; found %d.\n",
                    *numa_nodes);
            return -1;
        }
        if (config->thread_node < 0 && config->memory_node < 0) {
            config->thread_node = first;
            config->memory_node = second;
        } else if (config->thread_node < 0) {
            config->thread_node = config->memory_node == first ? second : first;
        } else if (config->memory_node < 0) {
            config->memory_node = config->thread_node == first ? second : first;
        }
        if (config->thread_node == config->memory_node) {
            fprintf(stderr, "Error: remote pattern requires different thread and memory nodes.\n");
            return -1;
        }
    } else if (placement_requested) {
        if (config->thread_node >= 0 && !numa_bitmask_isbitset(numa_all_nodes_ptr, config->thread_node)) {
            fprintf(stderr, "Error: requested thread NUMA node %d is unavailable.\n", config->thread_node);
            return -1;
        }
        if (config->memory_node >= 0 && !numa_bitmask_isbitset(numa_all_nodes_ptr, config->memory_node)) {
            fprintf(stderr, "Error: requested memory NUMA node %d is unavailable.\n", config->memory_node);
            return -1;
        }
    }
    if (config->thread_node >= 0 && !numa_bitmask_isbitset(numa_all_nodes_ptr, config->thread_node)) {
        fprintf(stderr, "Error: requested thread NUMA node %d is unavailable.\n", config->thread_node);
        return -1;
    }
    if (config->memory_node >= 0 && !numa_bitmask_isbitset(numa_all_nodes_ptr, config->memory_node)) {
        fprintf(stderr, "Error: requested memory NUMA node %d is unavailable.\n", config->memory_node);
        return -1;
    }
    return 0;
}

static int allocate_memory(const benchmark_config_t *config, size_t bytes, uint64_t **memory)
{
    void *allocation = NULL;

    if (config->memory_node >= 0) {
        numa_set_strict(1);
        allocation = numa_alloc_onnode(bytes, config->memory_node);
        if (allocation == NULL) {
            fprintf(stderr, "Error: libnuma could not allocate %zu bytes on node %d.\n",
                    bytes, config->memory_node);
            return -1;
        }
    } else if (posix_memalign(&allocation, 64, bytes) != 0) {
        fprintf(stderr, "Error: could not allocate %zu bytes.\n", bytes);
        return -1;
    }
    memset(allocation, 0, bytes);
    *memory = allocation;
    return 0;
}

static void free_memory(const benchmark_config_t *config, uint64_t *memory, size_t bytes)
{
    if (config->memory_node >= 0)
        numa_free(memory, bytes);
    else
        free(memory);
}

static int write_csv(const benchmark_config_t *config, int numa_nodes, uint64_t operations,
                     double execution_time, const char *timestamp)
{
    FILE *file;
    bool write_header = false;

    if (config->output_path == NULL)
        return 0;
    file = fopen(config->output_path, "a+");
    if (file == NULL) {
        fprintf(stderr, "Error: cannot open CSV output '%s': %s\n", config->output_path, strerror(errno));
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || ftell(file) == 0)
        write_header = true;
    if (write_header)
        fprintf(file, "timestamp,pattern,threads,memory_mb,iterations,duration_sec,thread_node,memory_node,numa_nodes,operations,execution_time_sec,throughput_ops_sec\n");
    fprintf(file, "%s,%s,%zu,%zu,%llu,%.6f,%d,%d,%d,%llu,%.6f,%.3f\n",
            timestamp, benchmark_pattern_name(config->pattern), config->threads, config->memory_mb,
            (unsigned long long)config->iterations, config->duration_set ? config->duration_sec : 0.0,
            config->thread_node, config->memory_node, numa_nodes, (unsigned long long)operations,
            execution_time, execution_time > 0.0 ? (double)operations / execution_time : 0.0);
    fclose(file);
    return 0;
}

static void current_timestamp(char *buffer, size_t buffer_size)
{
    time_t now = time(NULL);
    struct tm utc;

    gmtime_r(&now, &utc);
    strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

int main(int argc, char **argv)
{
    benchmark_config_t config = {
        .threads = DEFAULT_THREADS,
        .memory_mb = DEFAULT_MEMORY_MB,
        .iterations = DEFAULT_ITERATIONS,
        .duration_sec = 0.0,
        .duration_set = false,
        .pattern = PATTERN_SEQUENTIAL,
        .thread_node = -1,
        .memory_node = -1,
        .hot_percent = DEFAULT_HOT_PERCENT,
        .moderate_percent = DEFAULT_MODERATE_PERCENT,
        .cold_percent = DEFAULT_COLD_PERCENT,
        .change_phases = DEFAULT_CHANGE_PHASES,
        .seed = DEFAULT_SEED,
        .output_path = NULL
    };
    pthread_t *threads = NULL;
    worker_context_t *workers = NULL;
    worker_stats_t *stats = NULL;
    start_gate_t gate;
    atomic_int worker_error = 0;
    uint64_t *memory = NULL;
    size_t memory_bytes;
    size_t elements;
    uint64_t operations = 0;
    uint64_t checksum = 0;
    int numa_nodes;
    int parse_result;
    size_t created = 0;
    size_t i;
    struct timespec start_time;
    struct timespec end_time;
    double execution_time;
    char timestamp[32];
    int result = EXIT_FAILURE;

    parse_result = parse_options(argc, argv, &config);
    if (parse_result != 0)
        return parse_result > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    if (config.hot_percent + config.moderate_percent + config.cold_percent != 100U) {
        fprintf(stderr, "Error: hot, moderate, and cold percentages must sum to 100.\n");
        return EXIT_FAILURE;
    }
    if (config.change_phases < 2) {
        fprintf(stderr, "Error: --change-phases must be at least 2.\n");
        return EXIT_FAILURE;
    }
    if (config.threads > SIZE_MAX / (1024U * 1024U) ||
        config.memory_mb > SIZE_MAX / (1024U * 1024U)) {
        fprintf(stderr, "Error: requested size overflows size_t.\n");
        return EXIT_FAILURE;
    }
    memory_bytes = config.memory_mb * 1024U * 1024U;
    elements = memory_bytes / sizeof(uint64_t);
    if (elements < config.change_phases) {
        fprintf(stderr, "Error: memory allocation is too small for %u changing phases.\n", config.change_phases);
        return EXIT_FAILURE;
    }
    if (!config.duration_set && config.iterations > UINT64_MAX / config.threads) {
        fprintf(stderr, "Error: total operation count overflows uint64_t.\n");
        return EXIT_FAILURE;
    }
    if (validate_and_resolve_numa(&config, &numa_nodes) != 0)
        return EXIT_FAILURE;
    if (allocate_memory(&config, memory_bytes, &memory) != 0)
        return EXIT_FAILURE;

    threads = calloc(config.threads, sizeof(*threads));
    workers = calloc(config.threads, sizeof(*workers));
    stats = calloc(config.threads, sizeof(*stats));
    if (threads == NULL || workers == NULL || stats == NULL) {
        fprintf(stderr, "Error: could not allocate thread bookkeeping.\n");
        goto cleanup;
    }
    if (pthread_mutex_init(&gate.mutex, NULL) != 0 || pthread_cond_init(&gate.condition, NULL) != 0) {
        fprintf(stderr, "Error: could not initialize thread start gate.\n");
        goto cleanup;
    }
    gate.ready = 0;
    gate.start = false;
    gate.abort = false;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    for (i = 0; i < config.threads; i++) {
        workers[i].config = &config;
        workers[i].data = memory;
        workers[i].elements = elements;
        workers[i].memory_bytes = memory_bytes;
        workers[i].thread_id = i;
        workers[i].gate = &gate;
        workers[i].start_time = start_time;
        workers[i].worker_error = &worker_error;
        workers[i].stats = &stats[i];
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0)
            break;
        created++;
    }
    if (created != config.threads) {
        fprintf(stderr, "Error: pthread creation failed after %zu of %zu workers.\n", created, config.threads);
        pthread_mutex_lock(&gate.mutex);
        gate.abort = true;
        gate.start = true;
        pthread_cond_broadcast(&gate.condition);
        pthread_mutex_unlock(&gate.mutex);
        for (i = 0; i < created; i++)
            pthread_join(threads[i], NULL);
        pthread_cond_destroy(&gate.condition);
        pthread_mutex_destroy(&gate.mutex);
        goto cleanup;
    }
    pthread_mutex_lock(&gate.mutex);
    while (gate.ready < config.threads)
        pthread_cond_wait(&gate.condition, &gate.mutex);
    if (atomic_load(&worker_error) != 0)
        gate.abort = true;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    for (i = 0; i < config.threads; i++)
        workers[i].start_time = start_time;
    gate.start = true;
    pthread_cond_broadcast(&gate.condition);
    pthread_mutex_unlock(&gate.mutex);
    for (i = 0; i < config.threads; i++)
        pthread_join(threads[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    pthread_cond_destroy(&gate.condition);
    pthread_mutex_destroy(&gate.mutex);
    if (atomic_load(&worker_error) != 0) {
        fprintf(stderr, "Error: one or more workers could not establish requested CPU affinity.\n");
        goto cleanup;
    }

    for (i = 0; i < config.threads; i++) {
        operations += stats[i].operations;
        checksum ^= stats[i].checksum;
    }
    execution_time = timespec_seconds(&end_time) - timespec_seconds(&start_time);
    current_timestamp(timestamp, sizeof(timestamp));
    printf("AWAVMA Benchmark\n");
    printf("----------------\n");
    printf("Pattern          : %s\n", benchmark_pattern_name(config.pattern));
    printf("Threads          : %zu\n", config.threads);
    printf("Memory           : %zu MB\n", config.memory_mb);
    printf("Iterations       : %llu per thread%s\n", (unsigned long long)config.iterations,
           config.duration_set ? " (duration mode)" : "");
    printf("Duration         : %s\n", config.duration_set ? "enabled" : "not requested");
    printf("NUMA Nodes       : %d\n", numa_nodes);
    printf("Memory Node      : %d\n", config.memory_node);
    printf("Thread Node      : %d\n", config.thread_node);
    printf("Seed             : %llu\n", (unsigned long long)config.seed);
    printf("Regions          : hot=%u%% moderate=%u%% cold=%u%%\n",
           config.hot_percent, config.moderate_percent, config.cold_percent);
    printf("Starting benchmark...\n\n");
    printf("Completed successfully.\n\n");
    printf("Results\n");
    printf("-------\n");
    printf("Execution Time   : %.6f sec\n", execution_time);
    printf("Operations       : %llu\n", (unsigned long long)operations);
    printf("Throughput       : %.3f ops/sec\n",
           execution_time > 0.0 ? (double)operations / execution_time : 0.0);
    printf("Checksum         : %llu\n", (unsigned long long)checksum);
    for (i = 0; i < config.threads; i++)
        printf("Thread %zu        : %.6f sec, %llu ops, CPU %d (node %d)\n", i,
               stats[i].elapsed_sec, (unsigned long long)stats[i].operations,
               stats[i].cpu, stats[i].cpu_node);
    if (write_csv(&config, numa_nodes, operations, execution_time, timestamp) != 0)
        goto cleanup;
    result = EXIT_SUCCESS;

cleanup:
    free(stats);
    free(workers);
    free(threads);
    if (memory != NULL)
        free_memory(&config, memory, memory_bytes);
    return result;
}
