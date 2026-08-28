#ifndef AWAVMA_BENCHMARK_H
#define AWAVMA_BENCHMARK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PATTERN_SEQUENTIAL,
    PATTERN_RANDOM,
    PATTERN_HOT,
    PATTERN_MODERATE,
    PATTERN_COLD,
    PATTERN_MIXED,
    PATTERN_LOCAL,
    PATTERN_REMOTE,
    PATTERN_CHANGING
} benchmark_pattern_t;

typedef struct {
    size_t threads;
    size_t memory_mb;
    uint64_t iterations;
    double duration_sec;
    bool duration_set;
    benchmark_pattern_t pattern;
    int thread_node;
    int memory_node;
    unsigned hot_percent;
    unsigned moderate_percent;
    unsigned cold_percent;
    unsigned change_phases;
    uint64_t seed;
    const char *output_path;
} benchmark_config_t;

const char *benchmark_pattern_name(benchmark_pattern_t pattern);

#endif
