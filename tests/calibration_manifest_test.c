#define _POSIX_C_SOURCE 200809L
#include "calibration_manifest.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_manifest(const char *path, const char *mode, unsigned schema,
                           const char *hash, const char *experiment)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    fprintf(file, "{\"schema_version\":%u,\"kind\":\"AWAVMA_P4_CALIBRATION_AUTHORITY\","
            "\"mode\":\"%s\",\"status\":\"VALIDATED_PRODUCTION\",\"production_authority\":true,"
            "\"collection_experiment_id\":\"%s\",\"numa_balancing_restore_status\":\"RESTORED\","
            "\"timing_valid\":true,\"cost_migration_valid\":true,\"placement_validation_valid\":true,"
            "\"numa_balancing_transaction_valid\":true,\"strict_validation_valid\":true,\"overall_valid\":true,"
            "\"calibration_artifact_basename\":\"calibration.csv\",\"calibration_artifact_sha256\":\"%s\","
             "\"calibration_record_count\":1,\"calibration_version\":\"p4c-v2\",\"timing_metric\":\"throughput_ops_sec\","
             "\"topology_fingerprints\":[\"top1-0000000000000001\"],\"raw_artifacts\":[{\"path\":\"raw_cost.csv\",\"role\":\"COST\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"size_bytes\":0}]}\n",
            schema, mode, experiment, hash);
    assert(fclose(file) == 0);
}

int main(void)
{
    char directory[] = "/tmp/awavma-manifest-XXXXXX", artifact[256], manifest[256], reason[160];
    CalibrationRecord record = {0};
    CalibrationSnapshot snapshot = {.records = &record, .count = 1};
    FILE *file;
    assert(mkdtemp(directory) != NULL);
    snprintf(artifact, sizeof(artifact), "%s/calibration.csv", directory);
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", directory);
    file = fopen(artifact, "w"); assert(file != NULL); fputs("fixture\n", file); assert(fclose(file) == 0);
    record.status = CALIBRATION_VALIDATED_PRODUCTION;
    snprintf(record.calibration_version, sizeof(record.calibration_version), "p4c-v2");
    snprintf(record.collection_experiment_id, sizeof(record.collection_experiment_id), "experiment-1");
    snprintf(record.compatibility.topology_fingerprint, sizeof(record.compatibility.topology_fingerprint),
             "top1-0000000000000001");
    write_manifest(manifest, "full", 4, "e80b71cd14d3cbd65f4173abcbfcf01a545dbca32a72d575108b553a648cc96f", "experiment-1");
    assert(calibration_manifest_verify(artifact, manifest, &snapshot, reason));
    write_manifest(manifest, "smoke", 4, "e80b71cd14d3cbd65f4173abcbfcf01a545dbca32a72d575108b553a648cc96f", "experiment-1");
    assert(!calibration_manifest_verify(artifact, manifest, &snapshot, reason));
    write_manifest(manifest, "full", 2, "e80b71cd14d3cbd65f4173abcbfcf01a545dbca32a72d575108b553a648cc96f", "experiment-1");
    assert(!calibration_manifest_verify(artifact, manifest, &snapshot, reason));
    write_manifest(manifest, "full", 4, "0000000000000000000000000000000000000000000000000000000000000000", "experiment-1");
    assert(!calibration_manifest_verify(artifact, manifest, &snapshot, reason));
    write_manifest(manifest, "full", 4, "e80b71cd14d3cbd65f4173abcbfcf01a545dbca32a72d575108b553a648cc96f", "historical");
    assert(!calibration_manifest_verify(artifact, manifest, &snapshot, reason));
    unlink(manifest); unlink(artifact); rmdir(directory);
    puts("calibration_manifest_test: PASS");
    return 0;
}
