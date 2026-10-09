#ifndef AWAVMA_P5_C2B_LOCALITY_H
#define AWAVMA_P5_C2B_LOCALITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "benchmark_placement.h"

#define P5_C2B_SCHEMA_VERSION 1U
#define P5_C2B_MATRIX_VERSION "matrix-b-v1"
#define P5_C2B_METRIC "registered_memory_load_rate"
#define P5_C2B_METRIC_UNIT "ops/ms"
#define P5_C2B_WINDOW_INTERVALS 5U
#define P5_C2B_EXPECTED_UNITS 14U
#define P5_C2B_MINIMUM_UNITS 12U

typedef struct {
    const char *pattern;
    const char *placement;
} P5C2BCell;

typedef struct {
    size_t count;
    double minimum, maximum, mean, median, population_stddev, p10, p90;
} P5C2BStatistics;

size_t p5_c2b_matrix(P5C2BCell *cells, size_t capacity);
bool p5_c2b_rate(uint64_t operations, uint64_t interval_ms, double *rate);
bool p5_c2b_identifier_valid(const char *identifier);
bool p5_c2b_statistics(const double *values, size_t count, P5C2BStatistics *statistics);
bool p5_c2b_parse_placement_row(char *row, benchmark_placement_evidence_t *evidence);

#endif
