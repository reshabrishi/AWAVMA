#include "p5_c2b_locality.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t p5_c2b_matrix(P5C2BCell *cells, size_t capacity)
{
    static const P5C2BCell matrix[] = {
        {"cold", "LOCAL"}, {"cold", "REMOTE"},
        {"moderate", "LOCAL"}, {"moderate", "REMOTE"},
        {"hot", "LOCAL"}, {"hot", "REMOTE"}
    };
    if (cells == NULL || capacity < sizeof(matrix) / sizeof(matrix[0])) return 0;
    memcpy(cells, matrix, sizeof(matrix));
    return sizeof(matrix) / sizeof(matrix[0]);
}

bool p5_c2b_rate(uint64_t operations, uint64_t interval_ms, double *rate)
{
    if (interval_ms == 0 || rate == NULL) return false;
    *rate = (double)operations / (double)interval_ms;
    return true;
}

bool p5_c2b_identifier_valid(const char *identifier)
{
    size_t length;
    if (identifier == NULL || (length = strlen(identifier)) == 0 || length > 80) return false;
    for (size_t i = 0; i < length; ++i)
        if (!isalnum((unsigned char)identifier[i]) && identifier[i] != '-' && identifier[i] != '_') return false;
    return true;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static double percentile(const double *sorted, size_t count, double fraction)
{
    double position = fraction * (double)(count - 1), lower = floor(position);
    size_t index = (size_t)lower;
    return sorted[index] + (sorted[index + (index + 1 < count)] - sorted[index]) * (position - lower);
}

bool p5_c2b_statistics(const double *values, size_t count, P5C2BStatistics *statistics)
{
    double *sorted, sum = 0.0, squares = 0.0;
    if (values == NULL || count == 0 || statistics == NULL || (sorted = malloc(count * sizeof(*sorted))) == NULL) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!isfinite(values[i]) || values[i] < 0.0) { free(sorted); return false; }
        sorted[i] = values[i]; sum += values[i];
    }
    qsort(sorted, count, sizeof(*sorted), compare_double);
    *statistics = (P5C2BStatistics){.count = count, .minimum = sorted[0], .maximum = sorted[count - 1], .mean = sum / count,
        .median = percentile(sorted, count, 0.5), .p10 = percentile(sorted, count, 0.1), .p90 = percentile(sorted, count, 0.9)};
    for (size_t i = 0; i < count; ++i) { double difference = values[i] - statistics->mean; squares += difference * difference; }
    statistics->population_stddev = sqrt(squares / count);
    free(sorted); return true;
}

static size_t csv_fields(char *line, char **fields, size_t capacity)
{
    size_t count = 0; char *source = line, *target = line;
    while (*source && count < capacity) {
        bool quoted = *source == '"'; char delimiter;
        fields[count++] = target; if (quoted) source++;
        while (*source) {
            if (quoted && *source == '"' && source[1] == '"') { *target++ = '"'; source += 2; continue; }
            if ((quoted && *source == '"') || (!quoted && (*source == ',' || *source == '\n' || *source == '\r'))) break;
            *target++ = *source++;
        }
        if (quoted) { if (*source != '"') return 0; source++; }
        delimiter = *source;
        if (delimiter == ',') source++;
        else { while (*source == '\r' || *source == '\n') source++; if (*source) return 0; }
        *target++ = '\0';
        if (delimiter != ',') break;
    }
    return *source == '\0' ? count : 0;
}

static bool parse_int(const char *text, int *value)
{
    char *end; long parsed = strtol(text, &end, 10);
    if (end == text || *end) return false;
    *value = (int)parsed; return true;
}

static bool parse_size(const char *text, size_t *value)
{
    char *end; unsigned long long parsed = strtoull(text, &end, 10);
    if (end == text || *end) return false;
    *value = (size_t)parsed; return true;
}

bool p5_c2b_parse_placement_row(char *row, benchmark_placement_evidence_t *evidence)
{
    char *field[18];
    if (row == NULL || evidence == NULL) return false;
    memset(evidence, 0, sizeof(*evidence));
    if (csv_fields(row, field, 18) != 18 || strcmp(field[0], "1") ||
        (!strcmp(field[1], "local") ? (evidence->mode = BENCHMARK_PLACEMENT_LOCAL, false) :
         !strcmp(field[1], "remote") ? (evidence->mode = BENCHMARK_PLACEMENT_REMOTE, false) : true) ||
        !parse_int(field[2], &evidence->local_node) || !parse_int(field[3], &evidence->remote_node) ||
        !parse_int(field[4], &evidence->requested_memory_node) || !parse_int(field[5], &evidence->numa_distance) ||
        !parse_size(field[6], &evidence->total_pages) || !parse_size(field[7], &evidence->queryable_pages) ||
        !parse_size(field[8], &evidence->expected_node_pages) || !parse_size(field[9], &evidence->local_pages) ||
        !parse_size(field[10], &evidence->remote_pages) || !parse_size(field[11], &evidence->other_pages) ||
        !parse_size(field[12], &evidence->unknown_pages) || !parse_int(field[14], &evidence->observed_dominant_node) ||
        (strcmp(field[17], "true") && strcmp(field[17], "false"))) return false;
    evidence->memory_policy_restored = !strcmp(field[17], "true");
    snprintf(evidence->verification_status, sizeof(evidence->verification_status), "%s", field[15]);
    snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "%s", field[16]);
    return true;
}
