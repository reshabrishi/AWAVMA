#define _POSIX_C_SOURCE 200809L

#include "benefit_calibration.h"

#include "migration_target_provider.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const required_fields[] = {
    "schema_version", "calibration_id", "state", "provenance", "git_revision",
    "cpu_model", "socket_count", "numa_node_count", "numa_distance_fingerprint",
    "kernel_release", "source_node", "target_node", "workload", "threads", "memory_mb",
    "duration_seconds", "measured_runs_per_scenario", "local_mean_throughput",
    "remote_mean_throughput", "throughput_gain_percent", "local_mean_execution_time",
    "remote_mean_execution_time", "execution_time_improvement_percent", "environment_check_status",
    "validation_status", "created_at_utc"
};

static BenefitCalibrationState reject(BenefitCalibrationArtifact *artifact)
{
    if (artifact != NULL) {
        artifact->state = BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
        snprintf(artifact->provenance, sizeof(artifact->provenance), "invalid_benefit_calibration_artifact");
    }
    return BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
}

static bool parse_positive(const char *text, double *value)
{
    char *end = NULL;

    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' && isfinite(*value) && *value > 0.0;
}

static bool parse_finite(const char *text, double *value)
{
    char *end = NULL;

    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' && isfinite(*value);
}

static bool parse_index(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed >= MIGRATION_TARGET_MAX_NODES)
        return false;
    *value = (int)parsed;
    return true;
}

static bool parse_positive_integer(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0 || parsed > INT_MAX)
        return false;
    *value = (int)parsed;
    return true;
}

static bool current_cpu_model(char *model, size_t capacity)
{
    char line[512];
    FILE *file = fopen("/proc/cpuinfo", "r");

    if (file == NULL) return false;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strncmp(line, "model name", 10) == 0) {
            char *value = strchr(line, ':');
            size_t index = 0;

            if (value == NULL) break;
            value++;
            while (*value != '\0' && isspace((unsigned char)*value)) value++;
            while (*value != '\0' && *value != '\n' && index + 1 < capacity) {
                char current = *value++;
                model[index++] = current == ',' ? ' ' : current;
            }
            while (index > 0 && isspace((unsigned char)model[index - 1])) index--;
            model[index] = '\0';
            fclose(file);
            return index > 0;
        }
    fclose(file);
    return false;
}

static bool current_socket_count(int *count)
{
    char line[256];
    bool seen[256] = {false};
    int found = 0;
    FILE *file = fopen("/proc/cpuinfo", "r");

    if (file == NULL) return false;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strncmp(line, "physical id", 11) == 0) {
            char *value = strchr(line, ':');
            long socket = value == NULL ? -1 : strtol(value + 1, NULL, 10);

            if (socket >= 0 && socket < (long)sizeof(seen) && !seen[socket]) {
                seen[socket] = true;
                found++;
            }
        }
    fclose(file);
    *count = found == 0 ? 1 : found;
    return true;
}

static bool current_distance_fingerprint(const MigrationTargetTopology *topology, char *fingerprint,
                                         size_t capacity)
{
    unsigned long long hash = 1469598103934665603ULL;
    char path[128], distance[1024], canonical[1200];

    for (size_t node = 0; node < MIGRATION_TARGET_MAX_NODES; node++) {
        FILE *file;
        size_t length;

        if (!topology->node_present[node]) continue;
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%zu/distance", node);
        file = fopen(path, "r");
        if (file == NULL || fgets(distance, sizeof(distance), file) == NULL) {
            if (file != NULL) fclose(file);
            return false;
        }
        fclose(file);
        for (char *cursor = distance; *cursor != '\0'; cursor++)
            if (isspace((unsigned char)*cursor)) *cursor = ' ';
        length = (size_t)snprintf(canonical, sizeof(canonical), "node%zu:%s;", node, distance);
        if (length >= sizeof(canonical)) return false;
        for (size_t index = 0; index < length; index++) {
            hash ^= (unsigned char)canonical[index];
            hash *= 1099511628211ULL;
        }
    }
    return snprintf(fingerprint, capacity, "%016llx", hash) < (int)capacity;
}

BenefitCalibrationState benefit_calibration_load(const char *path, BenefitCalibrationArtifact *artifact)
{
    char header[2048], row[4096], *header_save = NULL, *row_save = NULL;
    char *headers[sizeof(required_fields) / sizeof(required_fields[0])];
    char *values[sizeof(required_fields) / sizeof(required_fields[0])];
    MigrationTargetTopology topology;
    FILE *file;
    size_t count = sizeof(required_fields) / sizeof(required_fields[0]);
    char model[512], fingerprint[32];
    int source, target, artifact_nodes, artifact_sockets, sockets;
    double number;

    if (artifact != NULL) {
        artifact->state = BENEFIT_CALIBRATION_UNAVAILABLE;
        artifact->provenance[0] = '\0';
    }
    if (path == NULL || *path == '\0')
        return BENEFIT_CALIBRATION_UNAVAILABLE;
    file = fopen(path, "r");
    if (file == NULL && errno == ENOENT)
        return BENEFIT_CALIBRATION_UNAVAILABLE;
    if (file == NULL)
        return reject(artifact);
    if (fgets(header, sizeof(header), file) == NULL || fgets(row, sizeof(row), file) == NULL ||
        strchr(row, '\n') == NULL || fgetc(file) != EOF) {
        fclose(file);
        return reject(artifact);
    }
    fclose(file);
    header[strcspn(header, "\r\n")] = '\0';
    row[strcspn(row, "\r\n")] = '\0';
    for (size_t index = 0; index < count; index++) {
        headers[index] = strtok_r(index == 0 ? header : NULL, ",", &header_save);
        values[index] = strtok_r(index == 0 ? row : NULL, ",", &row_save);
        if (headers[index] == NULL || values[index] == NULL || *values[index] == '\0' ||
            strcmp(headers[index], required_fields[index]) != 0)
            return reject(artifact);
    }
    if (strtok_r(NULL, ",", &header_save) != NULL || strtok_r(NULL, ",", &row_save) != NULL ||
        strcmp(values[0], "1") != 0 ||
        strcmp(values[2], "VALIDATED_PRODUCTION") != 0 || strcmp(values[23], "READY") != 0 ||
        strcmp(values[24], "PASS") != 0 ||
        !parse_positive_integer(values[6], &artifact_sockets) || !parse_positive_integer(values[7], &artifact_nodes) ||
        !parse_index(values[10], &source) || !parse_index(values[11], &target) || source == target ||
        !parse_positive(values[13], &number) || !parse_positive(values[14], &number) ||
        !parse_positive(values[15], &number) || !parse_positive(values[16], &number) ||
        !parse_positive(values[17], &number) || !parse_positive(values[18], &number) ||
        !parse_finite(values[19], &number) || !parse_positive(values[20], &number) ||
        !parse_positive(values[21], &number) || !parse_finite(values[22], &number) ||
        !migration_target_topology_read(&topology) ||
        !topology.node_present[source] || !topology.node_present[target] ||
        !current_cpu_model(model, sizeof(model)) || strcmp(values[5], model) != 0 ||
        !current_socket_count(&sockets) || artifact_sockets != sockets ||
        !current_distance_fingerprint(&topology, fingerprint, sizeof(fingerprint)) ||
        strcmp(values[8], fingerprint) != 0)
        return reject(artifact);
    {
        int nodes = 0;
        for (size_t index = 0; index < MIGRATION_TARGET_MAX_NODES; index++)
            nodes += topology.node_present[index] ? 1 : 0;
        if (nodes < 2 || artifact_nodes != nodes)
            return reject(artifact);
    }
    if (artifact != NULL) {
        /* Kernel release is retained as provenance; exact matching is unnecessarily fragile. */
        artifact->state = BENEFIT_CALIBRATION_VALIDATED_PRODUCTION;
        snprintf(artifact->provenance, sizeof(artifact->provenance), "%s", values[3]);
    }
    return BENEFIT_CALIBRATION_VALIDATED_PRODUCTION;
}
