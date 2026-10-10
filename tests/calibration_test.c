#include "calibration.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *header =
    "schema_version,calibration_version,calibration_id,created_at_utc,collection_experiment_id,calibration_status,"
    "cpu_architecture,cpu_model,online_numa_nodes,topology_fingerprint,local_node,remote_node,numa_distance,local_permitted_cpu_count,page_size_bytes,"
    "benchmark_pattern,threads,memory_bytes,memory_pages,duration_seconds,action_kind,source_node,destination_node,migration_page_bucket,"
    "valid_pair_count,local_mean_ms,local_stddev_ms,remote_mean_ms,remote_stddev_ms,paired_penalty_mean_ms,paired_penalty_lower_bound_ms,expected_recoverable_gain_pct,uncertainty_pct,safety_margin_pct,"
    "cost_sample_count,migration_cost_mean_ms,migration_cost_stddev_ms,migration_cost_conservative_ms,estimated_cost_pct,successful_pages,failed_pages,"
    "placement_evidence_schema_version,local_placement_artifact_hash,remote_placement_artifact_hash,migration_measurement_method,rejection_reason\n";

static CalibrationRecord record_for(ValidationAction action)
{
    CalibrationRecord record = {0};
    record.schema_version = CALIBRATION_SCHEMA_VERSION;
    snprintf(record.calibration_version, sizeof(record.calibration_version), "p4c-v2");
    snprintf(record.created_at_utc, sizeof(record.created_at_utc), "2026-10-05T00:00:00Z");
    snprintf(record.collection_experiment_id, sizeof(record.collection_experiment_id), "test-only");
    record.status = CALIBRATION_VALIDATED_TEST_ONLY;
    snprintf(record.compatibility.cpu_architecture, sizeof(record.compatibility.cpu_architecture), "x86_64");
    snprintf(record.compatibility.cpu_model, sizeof(record.compatibility.cpu_model), "test-cpu");
    record.compatibility.online_numa_nodes = 2; record.compatibility.local_node = 0;
    record.compatibility.remote_node = 1; record.compatibility.numa_distance = 21;
    record.compatibility.local_permitted_cpu_count = 4; record.compatibility.page_size_bytes = 4096;
    assert(calibration_topology_fingerprint(&record.compatibility, record.compatibility.topology_fingerprint));
    snprintf(record.workload.benchmark_pattern, sizeof(record.workload.benchmark_pattern), "mixed");
    record.workload.threads = 2; record.workload.memory_bytes = 4096U * 4096U;
    record.workload.memory_pages = 4096; record.workload.duration_seconds = 30.0;
    record.action = action; record.source_node = 1; record.destination_node = 0;
    record.migration_page_bucket = action == VALIDATION_ACTION_MOVE_MEMORY ? 4096 : 0;
    record.valid_pair_count = 7; record.local_mean_ms = 90; record.local_stddev_ms = 2;
    record.remote_mean_ms = 100; record.remote_stddev_ms = 3; record.paired_penalty_mean_ms = 10;
    record.paired_penalty_lower_bound_ms = 8; record.expected_recoverable_gain_pct = 8;
    record.uncertainty_pct = 1; record.safety_margin_pct = 1; record.cost_sample_count = 7;
    record.migration_cost_mean_ms = 4; record.migration_cost_stddev_ms = 1;
    record.migration_cost_conservative_ms = 5; record.estimated_cost_pct = 5;
    record.successful_pages = action == VALIDATION_ACTION_MOVE_MEMORY ? 4096 : 0;
    record.placement_evidence_schema_version = 1;
    snprintf(record.local_placement_artifact_hash, sizeof(record.local_placement_artifact_hash), "art1-0000000000000001");
    snprintf(record.remote_placement_artifact_hash, sizeof(record.remote_placement_artifact_hash), "art1-0000000000000002");
    snprintf(record.migration_measurement_method, sizeof(record.migration_measurement_method), action == VALIDATION_ACTION_MOVE_MEMORY ? "move_pages" : "sched_setaffinity");
    snprintf(record.rejection_reason, sizeof(record.rejection_reason), "none");
    assert(calibration_record_id(&record, record.calibration_id));
    return record;
}

static int write_record(const char *path, const CalibrationRecord *r)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) return -1;
    fputs(header, file);
    fprintf(file, "%u,%s,%s,%s,%s,%s,%s,%s,%u,%s,%d,%d,%d,%u,%zu,%s,%u,%zu,%zu,%.9g,%s,%d,%d,%zu,%zu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%zu,%.9g,%.9g,%.9g,%.9g,%zu,%zu,%u,%s,%s,%s,%s\n",
        r->schema_version, r->calibration_version, r->calibration_id, r->created_at_utc,
        r->collection_experiment_id, r->status == CALIBRATION_VALIDATED_PRODUCTION ? "VALIDATED_PRODUCTION" : "VALIDATED_TEST_ONLY",
        r->compatibility.cpu_architecture, r->compatibility.cpu_model, r->compatibility.online_numa_nodes,
        r->compatibility.topology_fingerprint, r->compatibility.local_node, r->compatibility.remote_node,
        r->compatibility.numa_distance, r->compatibility.local_permitted_cpu_count, r->compatibility.page_size_bytes,
        r->workload.benchmark_pattern, r->workload.threads, r->workload.memory_bytes, r->workload.memory_pages,
        r->workload.duration_seconds, r->action == VALIDATION_ACTION_MOVE_MEMORY ? "MOVE_MEMORY" : "MOVE_THREAD",
        r->source_node, r->destination_node, r->migration_page_bucket, r->valid_pair_count,
        r->local_mean_ms, r->local_stddev_ms, r->remote_mean_ms, r->remote_stddev_ms,
        r->paired_penalty_mean_ms, r->paired_penalty_lower_bound_ms, r->expected_recoverable_gain_pct,
        r->uncertainty_pct, r->safety_margin_pct, r->cost_sample_count, r->migration_cost_mean_ms,
        r->migration_cost_stddev_ms, r->migration_cost_conservative_ms, r->estimated_cost_pct,
        r->successful_pages, r->failed_pages, r->placement_evidence_schema_version,
        r->local_placement_artifact_hash, r->remote_placement_artifact_hash,
        r->migration_measurement_method, r->rejection_reason);
    return fclose(file);
}

int main(void)
{
    const char *production_path = "results/cloudlab_calibration/full/p4c-20261010T081928Z-3122/numa_calibration.csv";
    char path[] = "/tmp/awavma-calibration-XXXXXX";
    CalibrationRecord memory = record_for(VALIDATION_ACTION_MOVE_MEMORY);
    CalibrationSnapshot snapshot; CalibrationPolicy policy; calibration_match_status_t status;
    char reason[CALIBRATION_REASON_MAX]; ValidatedCalibrationMatch match;
    CalibrationMatchRequest request = {.compatibility = memory.compatibility, .workload = memory.workload,
                                       .action = VALIDATION_ACTION_MOVE_MEMORY, .source_node = 1,
                                       .destination_node = 0, .migration_page_bucket = 4096};
    int fd = mkstemp(path); assert(fd >= 0); close(fd); assert(write_record(path, &memory) == 0);
    calibration_policy_default(&policy);
    assert(calibration_load_csv(production_path, &policy, &snapshot, &status, reason) != 0);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) == 0 && snapshot.count == 1);
    assert(calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED && match.expected_gain_pct == 8 && match.effective_cost_pct == 7);
    assert(match.calibration_status == CALIBRATION_VALIDATED_TEST_ONLY);
    assert(strcmp(match.collection_experiment_id, "test-only") == 0);
    assert(match.placement_evidence_schema_version == 1);
    request.action = VALIDATION_ACTION_MOVE_THREAD;
    assert(calibration_match(&snapshot, &request, &match) == CALIBRATION_ACTION_MISMATCH);
    request.action = VALIDATION_ACTION_MOVE_MEMORY; request.migration_page_bucket = 64;
    assert(calibration_match(&snapshot, &request, &match) == CALIBRATION_BUCKET_MISMATCH);
    request.migration_page_bucket = 4096; request.destination_node = 1;
    assert(calibration_match(&snapshot, &request, &match) == CALIBRATION_MISMATCH);
    request.destination_node = 0; request.workload.threads++;
    assert(calibration_match(&snapshot, &request, &match) == CALIBRATION_WORKLOAD_MISMATCH);
    request.workload = memory.workload; request.compatibility.page_size_bytes = 8192;
    assert(calibration_match(&snapshot, &request, &match) == CALIBRATION_PAGE_SIZE_MISMATCH);
    request.compatibility = memory.compatibility; request.compatibility.cpu_model[0] = 'z';
    assert(calibration_match(&snapshot, &request, &match) == CALIBRATION_TOPOLOGY_MISMATCH);
    calibration_snapshot_release(&snapshot);
    memory.placement_evidence_schema_version = 2; assert(calibration_record_id(&memory, memory.calibration_id));
    assert(write_record(path, &memory) == 0);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) != 0 && status == CALIBRATION_MALFORMED);
    memory = record_for(VALIDATION_ACTION_MOVE_MEMORY); snprintf(memory.rejection_reason, sizeof(memory.rejection_reason), "partial");
    assert(calibration_record_id(&memory, memory.calibration_id)); assert(write_record(path, &memory) == 0);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) != 0 && status == CALIBRATION_MALFORMED);
    memory = record_for(VALIDATION_ACTION_MOVE_MEMORY); memory.failed_pages = 1; memory.successful_pages = 4095;
    assert(calibration_record_id(&memory, memory.calibration_id)); assert(write_record(path, &memory) == 0);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) != 0 && status == CALIBRATION_MALFORMED);
    memory = record_for(VALIDATION_ACTION_MOVE_MEMORY); memory.migration_cost_conservative_ms = 3; memory.estimated_cost_pct = 3;
    assert(calibration_record_id(&memory, memory.calibration_id)); assert(write_record(path, &memory) == 0);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) != 0 && status == CALIBRATION_MALFORMED);
    memory = record_for(VALIDATION_ACTION_MOVE_MEMORY); memory.source_node = memory.compatibility.local_node;
    assert(calibration_record_id(&memory, memory.calibration_id)); assert(write_record(path, &memory) == 0);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) != 0 && status == CALIBRATION_MALFORMED);
    memory = record_for(VALIDATION_ACTION_MOVE_MEMORY); snprintf(memory.calibration_version, sizeof(memory.calibration_version), "p4c-v1");
    assert(calibration_record_id(&memory, memory.calibration_id)); assert(write_record(path, &memory) == 0);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) != 0 && status == CALIBRATION_MALFORMED);
    memory = record_for(VALIDATION_ACTION_MOVE_MEMORY);
    memory.calibration_id[5] = 'f'; assert(write_record(path, &memory) == 0);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) != 0 && status == CALIBRATION_MALFORMED);
    FILE *file = fopen(path, "w"); assert(file != NULL); fputs("bad\n", file); fclose(file);
    assert(calibration_load_csv(path, &policy, &snapshot, &status, reason) != 0 && status == CALIBRATION_SCHEMA_UNSUPPORTED);
    unlink(path);
    printf("calibration_test: PASS\n");
    return 0;
}
