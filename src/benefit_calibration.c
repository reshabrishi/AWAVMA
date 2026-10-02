#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "benefit_calibration.h"

#include "sha256.h"

#include "migration_target_provider.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const required_fields[] = {
    "schema_version", "calibration_id", "state", "provenance", "git_revision",
    "cpu_model", "socket_count", "numa_node_count", "numa_distance_fingerprint",
    "kernel_release", "source_node", "target_node", "workload", "threads", "memory_mb",
    "duration_seconds", "measured_runs_per_scenario", "local_mean_throughput",
    "remote_mean_throughput", "throughput_gain_percent", "local_mean_execution_time",
    "remote_mean_execution_time", "execution_time_improvement_percent", "environment_check_status",
    "validation_status", "created_at_utc", "warmup_runs", "methodology_version",
    "schedule_version", "evidence_manifest_path", "evidence_manifest_sha256"
};

static BenefitCalibrationState reject(BenefitCalibrationArtifact *artifact)
{
    if (artifact != NULL) {
        artifact->state = BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
        artifact->source_node = -1;
        artifact->target_node = -1;
        artifact->throughput_gain_percent = NAN;
        artifact->execution_time_improvement_percent = NAN;
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

static bool lower_hex_sha256(const char *text)
{
    if (text == NULL || strlen(text) != 64) return false;
    for (size_t index = 0; index < 64; index++)
        if (!((text[index] >= '0' && text[index] <= '9') || (text[index] >= 'a' && text[index] <= 'f')))
            return false;
    return true;
}

static FILE *open_evidence_file(const char *root, const char *relative)
{
    char joined[PATH_MAX], resolved[PATH_MAX], prefix[PATH_MAX];
    struct stat status;

    if (root == NULL || relative == NULL || *relative == '\0' || relative[0] == '/' ||
        strstr(relative, "..") != NULL || strchr(relative, '\\') != NULL ||
        snprintf(joined, sizeof(joined), "%s/%s", root, relative) >= (int)sizeof(joined) ||
        lstat(joined, &status) != 0 || !S_ISREG(status.st_mode) || realpath(joined, resolved) == NULL ||
        snprintf(prefix, sizeof(prefix), "%s/", root) >= (int)sizeof(prefix) ||
        strncmp(resolved, prefix, strlen(prefix)) != 0)
        return NULL;
    return fopen(resolved, "rb");
}

static bool parse_native_evidence(FILE *file, int thread_node, int memory_node,
                                  double *execution, double *throughput)
{
    char header[2048], row[4096], *fields[16], *save = NULL;
    size_t count = 0;
    double value;
    int observed_thread, observed_memory;

    if (file == NULL || fgets(header, sizeof(header), file) == NULL ||
        fgets(row, sizeof(row), file) == NULL || fgetc(file) != EOF || strchr(row, '\n') == NULL)
        return false;
    row[strcspn(row, "\r\n")] = '\0';
    for (char *token = strtok_r(row, ",", &save); token != NULL && count < 16;
         token = strtok_r(NULL, ",", &save)) fields[count++] = token;
    if (count != 12 || strcmp(fields[1], "mixed") != 0 || strcmp(fields[2], "2") != 0 ||
        strcmp(fields[3], "1024") != 0 || !parse_positive(fields[5], &value) || value != 10.0 ||
        !parse_index(fields[6], &observed_thread) || !parse_index(fields[7], &observed_memory) ||
        observed_thread != thread_node || observed_memory != memory_node ||
        !parse_positive(fields[10], execution) || !parse_positive(fields[11], throughput))
        return false;
    return true;
}

static bool read_manifest_field(FILE *file, const char *key, char *value, size_t capacity)
{
    char line[4096];
    size_t key_length = strlen(key), length;

    if (fgets(line, sizeof(line), file) == NULL || strchr(line, '\r') != NULL ||
        strncmp(line, key, key_length) != 0 || line[key_length] != '=') return false;
    length = strcspn(line + key_length + 1, "\n");
    if (line[key_length + 1 + length] != '\n' || length == 0 || length >= capacity) return false;
    memcpy(value, line + key_length + 1, length); value[length] = '\0';
    return true;
}

static bool nearly_equal(double left, double right)
{
    double scale = fmax(1.0, fmax(fabs(left), fabs(right)));
    return fabs(left - right) <= 1e-9 * scale;
}

static bool verify_evidence(const char *artifact_path, const char *calibration_id, int local_node,
                            int remote_node, int node_count, const char *distance_fingerprint,
                            const char *cpu_model, int socket_count,
                            const char *manifest_digest, double expected_local_throughput,
                            double expected_remote_throughput, double expected_gain,
                            double expected_local_execution, double expected_remote_execution,
                            double expected_improvement)
{
    char root[PATH_MAX], resolved_root[PATH_MAX], manifest_path[PATH_MAX], value[4096], digest[65];
    const char *slash = strrchr(artifact_path, '/');
    FILE *manifest = NULL;
    double local_throughput = 0, remote_throughput = 0, local_execution = 0, remote_execution = 0;

    if (slash == NULL) snprintf(root, sizeof(root), ".");
    else if ((size_t)(slash - artifact_path) == 0) snprintf(root, sizeof(root), "/");
    else {
        size_t length = (size_t)(slash - artifact_path);
        if (length >= sizeof(root)) return false;
        memcpy(root, artifact_path, length); root[length] = '\0';
    }
    if (realpath(root, resolved_root) == NULL ||
        snprintf(manifest_path, sizeof(manifest_path), "%s/benefit_evidence.manifest", resolved_root) >= (int)sizeof(manifest_path) ||
        (manifest = open_evidence_file(resolved_root, "benefit_evidence.manifest")) == NULL ||
        !lower_hex_sha256(manifest_digest) || sha256_file_hex(manifest, digest) != 0 ||
        strcmp(digest, manifest_digest) != 0) {
        if (manifest != NULL) fclose(manifest);
        return false;
    }
    if (fgets(value, sizeof(value), manifest) == NULL || strcmp(value, "AWAVMA_BENEFIT_EVIDENCE_MANIFEST 1\n") != 0 ||
        !read_manifest_field(manifest, "calibration_id", value, sizeof(value)) || strcmp(value, calibration_id) != 0 ||
        !read_manifest_field(manifest, "methodology_version", value, sizeof(value)) || strcmp(value, "1") != 0 ||
        !read_manifest_field(manifest, "schedule_version", value, sizeof(value)) || strcmp(value, "1") != 0 ||
        !read_manifest_field(manifest, "local_node", value, sizeof(value)) || atoi(value) != local_node ||
        !read_manifest_field(manifest, "remote_node", value, sizeof(value)) || atoi(value) != remote_node ||
        !read_manifest_field(manifest, "numa_node_count", value, sizeof(value)) || atoi(value) != node_count ||
        !read_manifest_field(manifest, "numa_distance_fingerprint", value, sizeof(value)) || strcmp(value, distance_fingerprint) != 0 ||
        !read_manifest_field(manifest, "cpu_model", value, sizeof(value)) || strcmp(value, cpu_model) != 0 ||
        !read_manifest_field(manifest, "socket_count", value, sizeof(value)) || atoi(value) != socket_count ||
        !read_manifest_field(manifest, "workload", value, sizeof(value)) || strcmp(value, "mixed") != 0 ||
        !read_manifest_field(manifest, "threads", value, sizeof(value)) || strcmp(value, "2") != 0 ||
        !read_manifest_field(manifest, "memory_mb", value, sizeof(value)) || strcmp(value, "1024") != 0 ||
        !read_manifest_field(manifest, "duration_seconds", value, sizeof(value)) || strcmp(value, "10") != 0 ||
        !read_manifest_field(manifest, "warmup_runs", value, sizeof(value)) || strcmp(value, "2") != 0 ||
        !read_manifest_field(manifest, "measured_runs_per_scenario", value, sizeof(value)) || strcmp(value, "5") != 0 ||
        !read_manifest_field(manifest, "file", value, sizeof(value))) {
        fclose(manifest); return false;
    }
    {
        char *separator = strchr(value, '|');
        FILE *runs;
        if (separator == NULL || strcmp(value, "calibration_runs.csv|") == 0 ||
            strcmp(value, "calibration_runs.csv") == 0) { fclose(manifest); return false; }
        *separator++ = '\0';
        runs = strcmp(value, "calibration_runs.csv") == 0 ? open_evidence_file(resolved_root, value) : NULL;
        if (runs == NULL || !lower_hex_sha256(separator) || sha256_file_hex(runs, digest) != 0 || strcmp(digest, separator) != 0) {
            if (runs != NULL)
                fclose(runs);
            fclose(manifest);
            return false;
        }
        fclose(runs);
    }
    for (int index = 0; index < 14; index++) {
        char expected_id[48], expected_role[9], expected_condition[7], expected_path[80];
        char *parts[7], *save = NULL, *token;
        int expected_thread, expected_memory;
        FILE *raw = NULL;
        double execution, throughput;

        if (!read_manifest_field(manifest, "run", value, sizeof(value))) { fclose(manifest); return false; }
        token = strtok_r(value, "|", &save);
        for (size_t part = 0; part < 7; part++) {
            parts[part] = token;
            if (token == NULL) { fclose(manifest); return false; }
            token = strtok_r(NULL, "|", &save);
        }
        if (token != NULL || atoi(parts[0]) != index + 1) { fclose(manifest); return false; }
        if (index < 2) {
            snprintf(expected_id, sizeof(expected_id), "warmup-local-%02d", index + 1);
            snprintf(expected_role, sizeof(expected_role), "WARMUP"); snprintf(expected_condition, sizeof(expected_condition), "LOCAL");
        } else if (index < 4) {
            snprintf(expected_id, sizeof(expected_id), "warmup-remote-%02d", index - 1);
            snprintf(expected_role, sizeof(expected_role), "WARMUP"); snprintf(expected_condition, sizeof(expected_condition), "REMOTE");
        } else {
            int pair = (index - 4) / 2 + 1;
            snprintf(expected_id, sizeof(expected_id), "measured-%s-%02d", index % 2 == 0 ? "local" : "remote", pair);
            snprintf(expected_role, sizeof(expected_role), "MEASURED"); snprintf(expected_condition, sizeof(expected_condition), index % 2 == 0 ? "LOCAL" : "REMOTE");
        }
        snprintf(expected_path, sizeof(expected_path), "raw/%s.csv", expected_id);
        if (strcmp(parts[1], expected_id) != 0 || strcmp(parts[2], expected_role) != 0 ||
            strcmp(parts[3], expected_condition) != 0 || strcmp(parts[4], expected_path) != 0 ||
            !lower_hex_sha256(parts[5]) || strcmp(parts[6], expected_id) != 0 ||
            (raw = open_evidence_file(resolved_root, parts[4])) == NULL ||
            sha256_file_hex(raw, digest) != 0 || strcmp(digest, parts[5]) != 0) {
            if (raw != NULL)
                fclose(raw);
            fclose(manifest);
            return false;
        }
        expected_thread = strcmp(expected_condition, "LOCAL") == 0 ? local_node : remote_node;
        expected_memory = local_node;
        if (!parse_native_evidence(raw, expected_thread, expected_memory, &execution, &throughput)) {
            fclose(raw); fclose(manifest); return false;
        }
        fclose(raw);
        if (strcmp(expected_role, "MEASURED") == 0) {
            if (strcmp(expected_condition, "LOCAL") == 0) { local_throughput += throughput; local_execution += execution; }
            else { remote_throughput += throughput; remote_execution += execution; }
        }
    }
    if (fgetc(manifest) != EOF) { fclose(manifest); return false; }
    fclose(manifest);
    local_throughput /= 5.0; remote_throughput /= 5.0; local_execution /= 5.0; remote_execution /= 5.0;
    return nearly_equal(local_throughput, expected_local_throughput) &&
           nearly_equal(remote_throughput, expected_remote_throughput) &&
           nearly_equal((local_throughput - remote_throughput) / remote_throughput * 100.0, expected_gain) &&
           nearly_equal(local_execution, expected_local_execution) && nearly_equal(remote_execution, expected_remote_execution) &&
           nearly_equal((remote_execution - local_execution) / remote_execution * 100.0, expected_improvement);
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
    int source, target, artifact_nodes, artifact_sockets, measured_runs, warmup_runs, methodology_version,
        schedule_version, sockets;
    double number, local_throughput, remote_throughput, throughput_gain, local_execution, remote_execution,
        execution_improvement;

    if (artifact != NULL) {
        artifact->state = BENEFIT_CALIBRATION_UNAVAILABLE;
        artifact->source_node = -1;
        artifact->target_node = -1;
        artifact->throughput_gain_percent = NAN;
        artifact->execution_time_improvement_percent = NAN;
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
        strcmp(values[0], "3") != 0 ||
        strcmp(values[2], "VALIDATED_PRODUCTION") != 0 || strcmp(values[23], "READY") != 0 ||
        strcmp(values[24], "PASS") != 0 ||
        !parse_positive_integer(values[6], &artifact_sockets) || !parse_positive_integer(values[7], &artifact_nodes) ||
        !parse_index(values[10], &source) || !parse_index(values[11], &target) || source == target ||
        strcmp(values[12], "mixed") != 0 || !parse_positive(values[13], &number) || number != 2.0 ||
        !parse_positive(values[14], &number) || number != 1024.0 ||
        !parse_positive(values[15], &number) || number != 10.0 ||
        !parse_positive_integer(values[16], &measured_runs) || measured_runs != 5 ||
        !parse_positive(values[17], &local_throughput) || !parse_positive(values[18], &remote_throughput) ||
        !parse_finite(values[19], &throughput_gain) || !parse_positive(values[20], &number) ||
        !parse_positive(values[21], &number) || !parse_finite(values[22], &execution_improvement) ||
        !parse_positive_integer(values[26], &warmup_runs) || warmup_runs != 2 ||
        !parse_positive_integer(values[27], &methodology_version) || methodology_version != 1 ||
        !parse_positive_integer(values[28], &schedule_version) || schedule_version != 1 ||
        strcmp(values[29], "benefit_evidence.manifest") != 0 || !lower_hex_sha256(values[30]) ||
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
    if (!parse_positive(values[20], &local_execution) || !parse_positive(values[21], &remote_execution) ||
        !verify_evidence(path, values[1], target, source, artifact_nodes, values[8], values[5], artifact_sockets, values[30],
                         local_throughput, remote_throughput, throughput_gain, local_execution,
                         remote_execution, execution_improvement))
        return reject(artifact);
    if (artifact != NULL) {
        /* Kernel release is retained as provenance; exact matching is unnecessarily fragile. */
        artifact->state = BENEFIT_CALIBRATION_VALIDATED_PRODUCTION;
        artifact->source_node = source;
        artifact->target_node = target;
        artifact->throughput_gain_percent = throughput_gain;
        artifact->execution_time_improvement_percent = execution_improvement;
        snprintf(artifact->provenance, sizeof(artifact->provenance), "%s", values[3]);
    }
    return BENEFIT_CALIBRATION_VALIDATED_PRODUCTION;
}
