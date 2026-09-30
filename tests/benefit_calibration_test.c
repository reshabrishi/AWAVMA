#define _POSIX_C_SOURCE 200809L

#include "benefit_calibration.h"
#include "migration_target_provider.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void report(const char *id, int passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
}

static int machine_fields(const MigrationTargetTopology *topology, char *model, size_t model_capacity,
                          int *sockets, char *fingerprint, size_t fingerprint_capacity)
{
    char line[512], path[128], distance[1024], canonical[1200];
    bool seen[256] = {false};
    unsigned long long hash = 1469598103934665603ULL;
    FILE *file = fopen("/proc/cpuinfo", "r");
    size_t index = 0;

    if (file == NULL) return 0;
    *sockets = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "model name", 10) == 0 && index == 0) {
            char *value = strchr(line, ':');
            if (value == NULL) { fclose(file); return 0; }
            value++; while (*value != '\0' && isspace((unsigned char)*value)) value++;
            while (*value != '\0' && *value != '\n' && index + 1 < model_capacity) {
                char current = *value++; model[index++] = current == ',' ? ' ' : current;
            }
            model[index] = '\0';
        } else if (strncmp(line, "physical id", 11) == 0) {
            long socket = strtol(strchr(line, ':') + 1, NULL, 10);
            if (socket >= 0 && socket < 256 && !seen[socket]) { seen[socket] = true; (*sockets)++; }
        }
    }
    fclose(file);
    if (*sockets == 0) *sockets = 1;
    for (int node = 0; node < MIGRATION_TARGET_MAX_NODES; node++) if (topology->node_present[node]) {
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/distance", node);
        file = fopen(path, "r");
        if (file == NULL || fgets(distance, sizeof(distance), file) == NULL) { if (file != NULL) fclose(file); return 0; }
        fclose(file);
        for (char *cursor = distance; *cursor != '\0'; cursor++) if (isspace((unsigned char)*cursor)) *cursor = ' ';
        int length = snprintf(canonical, sizeof(canonical), "node%d:%s;", node, distance);
        if (length < 0 || length >= (int)sizeof(canonical)) return 0;
        for (int offset = 0; offset < length; offset++) { hash ^= (unsigned char)canonical[offset]; hash *= 1099511628211ULL; }
    }
    return index > 0 && snprintf(fingerprint, fingerprint_capacity, "%016llx", hash) < (int)fingerprint_capacity;
}

static int write_artifact(const char *path, int nodes, int source, int target, const char *schema,
                          const char *local_mean, const char *model, int sockets, const char *fingerprint)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) return 0;
    fprintf(file, "schema_version,calibration_id,state,provenance,git_revision,cpu_model,socket_count,numa_node_count,numa_distance_fingerprint,kernel_release,source_node,target_node,workload,threads,memory_mb,duration_seconds,measured_runs_per_scenario,local_mean_throughput,remote_mean_throughput,throughput_gain_percent,local_mean_execution_time,remote_mean_execution_time,execution_time_improvement_percent,environment_check_status,validation_status,created_at_utc\n");
    fprintf(file, "%s,fixture,VALIDATED_PRODUCTION,synthetic_loader_fixture,revision,%s,%d,%d,%s,kernel,%d,%d,mixed,2,1024,10,5,%s,90,10,10,11,10,READY,PASS,2026-09-30T00:00:00Z\n",
            schema, model, sockets, nodes, fingerprint, source, target, local_mean);
    return fclose(file) == 0;
}

int main(void)
{
    char root[] = "/tmp/awavma-benefit-calibration-XXXXXX";
    char artifact[512];
    MigrationTargetTopology topology;
    BenefitCalibrationArtifact loaded;
    char model[512], fingerprint[32];
    int sockets;
    int nodes = 0, source = -1, target = -1;
    int passed = 1;

    if (mkdtemp(root) == NULL || !migration_target_topology_read(&topology) ||
        !machine_fields(&topology, model, sizeof(model), &sockets, fingerprint, sizeof(fingerprint))) return EXIT_FAILURE;
    for (int index = 0; index < MIGRATION_TARGET_MAX_NODES; index++)
        if (topology.node_present[index]) {
            nodes++;
            if (source < 0) source = index;
            else if (target < 0) target = index;
        }
    snprintf(artifact, sizeof(artifact), "%s/artifact.csv", root);
    passed &= benefit_calibration_load("/missing/benefit-calibration.csv", &loaded) == BENEFIT_CALIBRATION_UNAVAILABLE;
    report("BCL01_MISSING_UNAVAILABLE", passed);
    FILE *bad = fopen(artifact, "w"); if (bad != NULL) { fputs("broken\n", bad); fclose(bad); }
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL02_MALFORMED", passed);
    bad = fopen(artifact, "w"); if (bad != NULL) { fputs("schema_version,calibration_id\n", bad); fclose(bad); }
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL03_INCOMPLETE", passed);
    passed &= write_artifact(artifact, nodes, source, target, "2", "100", model, sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL04_WRONG_SCHEMA", passed);
    passed &= write_artifact(artifact, nodes, source, source, "1", "100", model, sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL05_SOURCE_EQUALS_TARGET", passed);
    passed &= write_artifact(artifact, nodes, source, target, "1", "nan", model, sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL06_NONFINITE", passed);
    passed &= write_artifact(artifact, nodes + 1, source, target, "1", "100", model, sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL07_TOPOLOGY_MISMATCH", passed);
    passed &= write_artifact(artifact, nodes, source, target, "1", "100", "other-cpu", sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL09_CPU_MODEL_MISMATCH", passed);
    passed &= write_artifact(artifact, nodes, source, target, "1", "100", model, sockets + 1, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL10_SOCKET_COUNT_MISMATCH", passed);
    passed &= write_artifact(artifact, nodes, source, target, "1", "100", model, sockets, "0000000000000000");
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL11_DISTANCE_FINGERPRINT_MISMATCH", passed);
    if (nodes >= 2) {
        passed &= write_artifact(artifact, nodes, source, target, "1", "100", model, sockets, fingerprint);
        passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_VALIDATED_PRODUCTION;
        report("BCL08_VALID_SYNTHETIC_FIXTURE", passed);
    }
    unlink(artifact); rmdir(root);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
