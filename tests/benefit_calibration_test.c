#define _POSIX_C_SOURCE 200809L

#include "benefit_calibration.h"
#include "migration_target_provider.h"
#include "sha256.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static int write_artifact_with_methodology(const char *path, int nodes, int source, int target,
                                           const char *schema, const char *local_mean,
                                           const char *throughput_gain, const char *execution_improvement,
                                           const char *model, int sockets, const char *fingerprint,
                                           const char *workload, const char *threads,
                                           const char *memory_mb, const char *duration_seconds,
                                           const char *runs, const char *warmups)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) return 0;
    fprintf(file, "schema_version,calibration_id,state,provenance,git_revision,cpu_model,socket_count,numa_node_count,numa_distance_fingerprint,kernel_release,source_node,target_node,workload,threads,memory_mb,duration_seconds,measured_runs_per_scenario,local_mean_throughput,remote_mean_throughput,throughput_gain_percent,local_mean_execution_time,remote_mean_execution_time,execution_time_improvement_percent,environment_check_status,validation_status,created_at_utc,warmup_runs\n");
    fprintf(file, "%s,fixture,VALIDATED_PRODUCTION,synthetic_loader_fixture,revision,%s,%d,%d,%s,kernel,%d,%d,%s,%s,%s,%s,%s,%s,90,%s,10,11,%s,READY,PASS,2026-09-30T00:00:00Z,%s\n",
            schema, model, sockets, nodes, fingerprint, source, target,
            workload, threads, memory_mb, duration_seconds, runs, local_mean, throughput_gain,
            execution_improvement, warmups);
    return fclose(file) == 0;
}

static int write_artifact_with_benefit(const char *path, int nodes, int source, int target,
                                       const char *schema, const char *local_mean,
                                       const char *throughput_gain, const char *execution_improvement,
                                       const char *model, int sockets, const char *fingerprint)
{
    return write_artifact_with_methodology(path, nodes, source, target, schema, local_mean,
                                           throughput_gain, execution_improvement, model, sockets,
                                           fingerprint, "mixed", "2", "1024", "10", "5", "2");
}

static int write_artifact(const char *path, int nodes, int source, int target, const char *schema,
                          const char *local_mean, const char *model, int sockets, const char *fingerprint)
{
    return write_artifact_with_benefit(path, nodes, source, target, schema, local_mean, "10", "10",
                                       model, sockets, fingerprint);
}

static int write_native(const char *path, int thread_node, int memory_node, double execution, double throughput)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) return 0;
    fprintf(file, "timestamp,pattern,threads,memory_mb,iterations,duration_sec,thread_node,memory_node,numa_nodes,operations,execution_time_sec,throughput_ops_sec\n");
    fprintf(file, "0,mixed,2,1024,1,10,%d,%d,2,100,%.17g,%.17g\n", thread_node, memory_node,
            execution, throughput);
    return fclose(file) == 0;
}

static int write_bound_artifact(const char *path, int nodes, int source, int target, const char *model,
                                int sockets, const char *fingerprint, double local_throughput,
                                double remote_throughput)
{
    static const char *const ids[] = {"warmup-local-01", "warmup-local-02", "warmup-remote-01", "warmup-remote-02",
                                      "measured-local-01", "measured-remote-01", "measured-local-02", "measured-remote-02",
                                      "measured-local-03", "measured-remote-03", "measured-local-04", "measured-remote-04",
                                      "measured-local-05", "measured-remote-05"};
    char root[512], raw[512], native[512], manifest[512], digest[65], raw_digest[65];
    FILE *file;
    double local_execution = 10.0, remote_execution = 11.0;
    double gain = (local_throughput - remote_throughput) / remote_throughput * 100.0;
    double improvement = (remote_execution - local_execution) / remote_execution * 100.0;

    snprintf(root, sizeof(root), "%s", path);
    char *slash = strrchr(root, '/');
    if (slash == NULL) return 0;
    *slash = '\0';
    snprintf(raw, sizeof(raw), "%s/raw", root);
    for (int index = 0; index < 14; index++) {
        snprintf(native, sizeof(native), "%s/%s.csv", raw, ids[index]);
        unlink(native);
    }
    snprintf(native, sizeof(native), "%s/calibration_runs.csv", root); unlink(native);
    snprintf(manifest, sizeof(manifest), "%s/benefit_evidence.manifest", root); unlink(manifest);
    rmdir(raw);
    if (mkdir(raw, 0700) != 0) return 0;
    for (int index = 0; index < 14; index++) {
        int local = index < 2 || (index >= 4 && index % 2 == 0);
        double throughput = index < 4 ? (local ? 10000.0 : 1.0) : (local ? local_throughput : remote_throughput);
        double execution = index < 4 ? (local ? 1.0 : 99.0) : (local ? local_execution : remote_execution);

        snprintf(native, sizeof(native), "%s/%s.csv", raw, ids[index]);
        if (!write_native(native, local ? target : source, target, execution, throughput)) return 0;
    }
    snprintf(native, sizeof(native), "%s/calibration_runs.csv", root);
    file = fopen(native, "w");
    if (file == NULL) return 0;
    fputs("logical_run_id\n", file);
    for (int index = 0; index < 14; index++) fprintf(file, "%s\n", ids[index]);
    if (fclose(file) != 0) return 0;
    snprintf(manifest, sizeof(manifest), "%s/benefit_evidence.manifest", root);
    file = fopen(manifest, "w");
    if (file == NULL) return 0;
    fprintf(file, "AWAVMA_BENEFIT_EVIDENCE_MANIFEST 1\ncalibration_id=fixture\nmethodology_version=1\nschedule_version=1\nlocal_node=%d\nremote_node=%d\nnuma_node_count=%d\nnuma_distance_fingerprint=%s\ncpu_model=%s\nsocket_count=%d\nworkload=mixed\nthreads=2\nmemory_mb=1024\nduration_seconds=10\nwarmup_runs=2\nmeasured_runs_per_scenario=5\n", target, source, nodes, fingerprint, model, sockets);
    if (fclose(file) != 0) return 0;
    file = fopen(native, "rb"); if (file == NULL || sha256_file_hex(file, raw_digest) != 0) { if (file != NULL) fclose(file); return 0; }
    fclose(file); file = fopen(manifest, "a"); if (file == NULL) return 0;
    fprintf(file, "file=calibration_runs.csv|%s\n", raw_digest); fclose(file);
    for (int index = 0; index < 14; index++) {
        const char *role = index < 4 ? "WARMUP" : "MEASURED";
        const char *condition = (index < 2 || (index >= 4 && index % 2 == 0)) ? "LOCAL" : "REMOTE";
        snprintf(native, sizeof(native), "%s/%s.csv", raw, ids[index]);
        file = fopen(native, "rb"); if (file == NULL || sha256_file_hex(file, raw_digest) != 0) { if (file != NULL) fclose(file); return 0; }
        fclose(file); file = fopen(manifest, "a"); if (file == NULL) return 0;
        fprintf(file, "run=%d|%s|%s|%s|raw/%s.csv|%s|%s\n", index + 1, ids[index], role, condition, ids[index], raw_digest, ids[index]);
        fclose(file);
    }
    file = fopen(manifest, "rb"); if (file == NULL || sha256_file_hex(file, digest) != 0) { if (file != NULL) fclose(file); return 0; }
    fclose(file);
    file = fopen(path, "w");
    if (file == NULL) return 0;
    fprintf(file, "schema_version,calibration_id,state,provenance,git_revision,cpu_model,socket_count,numa_node_count,numa_distance_fingerprint,kernel_release,source_node,target_node,workload,threads,memory_mb,duration_seconds,measured_runs_per_scenario,local_mean_throughput,remote_mean_throughput,throughput_gain_percent,local_mean_execution_time,remote_mean_execution_time,execution_time_improvement_percent,environment_check_status,validation_status,created_at_utc,warmup_runs,methodology_version,schedule_version,evidence_manifest_path,evidence_manifest_sha256\n");
    fprintf(file, "3,fixture,VALIDATED_PRODUCTION,synthetic_bound_fixture,revision,%s,%d,%d,%s,kernel,%d,%d,mixed,2,1024,10,5,%.17g,%.17g,%.17g,10,11,%.17g,READY,PASS,2026-09-30T00:00:00Z,2,1,1,benefit_evidence.manifest,%s\n",
            model, sockets, nodes, fingerprint, source, target, local_throughput, remote_throughput, gain,
            improvement, digest);
    return fclose(file) == 0;
}

static int loader_authorizes_thread_route(const BenefitCalibrationArtifact *artifact, int source_node,
                                          int target_node)
{
    DecisionData phase5 = {0};
    ValidationResult phase6 = {0};
    MigrationTarget target = {0};
    BenefitClassifierInput input = {0};
    BenefitDecision decision;

    phase5.pid = 42; phase5.action = VALIDATION_ACTION_MOVE_THREAD;
    phase5.evidence_model = DECISION_EVIDENCE_EMPIRICAL_GAIN_COST;
    phase5.gain_available = true; phase5.cost_available = true;
    phase5.predicted_gain = 2.0; phase5.estimated_cost = 1.0;
    phase6.pid = 42; phase6.action = VALIDATION_ACTION_MOVE_THREAD;
    phase6.confidence_status = GATE_PASS; phase6.roi_status = GATE_PASS;
    phase6.safety_status = GATE_PASS; phase6.confidence_score = 80.0;
    phase6.roi_score = 10.0; phase6.safety_score = 80.0;
    snprintf(phase6.final_decision, sizeof(phase6.final_decision), "APPROVED");
    target.pid = 42; target.start_time_ticks = 99; target.action = VALIDATION_ACTION_MOVE_THREAD;
    snprintf(target.attempt_id, sizeof(target.attempt_id), "artifact-attempt");
    target.source_node_known = true; target.source_numa_node = source_node;
    target.has_target_numa_node = true; target.target_numa_node = target_node;
    target.has_target_cpu_mask = true; CPU_SET(0, &target.target_cpu_mask);
    input.pid = 42; input.start_time_ticks = 99; input.attempt_id = "artifact-attempt";
    input.action = VALIDATION_ACTION_MOVE_THREAD; input.process_active = true;
    input.identity_match = true; input.phase5_fresh = true; input.phase6_fresh = true;
    input.evidence_attempt_bound = true; input.decision = &phase5; input.validation = &phase6;
    input.target = &target; input.target_provider_validated = true; input.target_online = true;
    input.target_permitted = true; input.source_known = true; input.source_target_valid = true;
    input.calibration.state = artifact->state; input.calibration.provenance = artifact->provenance;
    input.calibration.source_node = artifact->source_node; input.calibration.target_node = artifact->target_node;
    input.calibration.throughput_gain_percent = artifact->throughput_gain_percent;
    input.calibration.execution_time_improvement_percent = artifact->execution_time_improvement_percent;
    return benefit_classifier_evaluate(&input, &decision) == BENEFIT_SUPPORTED;
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
    passed &= write_artifact(artifact, nodes, source, target, "1", "100", model, sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL04_WRONG_SCHEMA", passed);
    passed &= write_artifact(artifact, nodes, source, source, "2", "100", model, sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL05_SOURCE_EQUALS_TARGET", passed);
    passed &= write_artifact(artifact, nodes, source, target, "1", "nan", model, sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL06_NONFINITE", passed);
    passed &= write_artifact(artifact, nodes + 1, source, target, "2", "100", model, sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL07_TOPOLOGY_MISMATCH", passed);
    passed &= write_artifact(artifact, nodes, source, target, "2", "100", "other-cpu", sockets, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL09_CPU_MODEL_MISMATCH", passed);
    passed &= write_artifact(artifact, nodes, source, target, "2", "100", model, sockets + 1, fingerprint);
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL10_SOCKET_COUNT_MISMATCH", passed);
    passed &= write_artifact(artifact, nodes, source, target, "2", "100", model, sockets, "0000000000000000");
    passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
    report("BCL11_DISTANCE_FINGERPRINT_MISMATCH", passed);
    if (nodes >= 2) {
        passed &= write_bound_artifact(artifact, nodes, source, target, model, sockets, fingerprint, 100.0, 90.0);
        passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_VALIDATED_PRODUCTION &&
                  loaded.source_node == source && loaded.target_node == target &&
                  loaded.throughput_gain_percent > 0.0 &&
                  loaded.execution_time_improvement_percent > 0.0;
        report("BCL08_VALID_SYNTHETIC_FIXTURE", passed);
        passed &= loader_authorizes_thread_route(&loaded, source, target);
        report("BCL08B_LOADER_TO_MATCHING_THREAD_AUTHORIZATION", passed);
        passed &= !loader_authorizes_thread_route(&loaded, target, source);
        report("BCL08C_REVERSE_THREAD_ROUTE_REJECTED", passed);
        for (int alternate = 0; alternate < MIGRATION_TARGET_MAX_NODES; alternate++)
            if (topology.node_present[alternate] && alternate != source && alternate != target) {
                passed &= !loader_authorizes_thread_route(&loaded, source, alternate);
                report("BCL08D_ALTERNATE_THREAD_ROUTE_REJECTED", passed);
                break;
            }
        {
            char manifest[512], raw_file[512];

            snprintf(manifest, sizeof(manifest), "%s/benefit_evidence.manifest", root);
            unlink(manifest);
            passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
            report("BCL16_MISSING_EVIDENCE_MANIFEST_REJECTED", passed);
            passed &= write_bound_artifact(artifact, nodes, source, target, model, sockets, fingerprint, 100.0, 90.0);
            snprintf(raw_file, sizeof(raw_file), "%s/raw/measured-local-01.csv", root);
            unlink(raw_file);
            passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
            report("BCL17_MISSING_RAW_EVIDENCE_REJECTED", passed);
            passed &= write_bound_artifact(artifact, nodes, source, target, model, sockets, fingerprint, 100.0, 90.0);
            bad = fopen(raw_file, "a"); if (bad != NULL) { fputs("tampered\n", bad); fclose(bad); }
            passed &= benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
            report("BCL18_TAMPERED_RAW_EVIDENCE_REJECTED", passed);
        }
        passed &= write_bound_artifact(artifact, nodes, source, target, model, sockets, fingerprint, 100.0, 100.0) &&
                   benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_VALIDATED_PRODUCTION &&
                   loaded.throughput_gain_percent == 0.0 &&
                   !loader_authorizes_thread_route(&loaded, source, target);
        report("BCL12_ZERO_GAIN_VALID_ARTIFACT", passed);
        passed &= write_bound_artifact(artifact, nodes, source, target, model, sockets, fingerprint, 90.0, 100.0) &&
                   benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_VALIDATED_PRODUCTION &&
                   loaded.throughput_gain_percent < 0.0 &&
                   !loader_authorizes_thread_route(&loaded, source, target);
        report("BCL13_NEGATIVE_GAIN_VALID_ARTIFACT", passed);
        passed &= write_artifact_with_benefit(artifact, nodes, source, target, "2", "100", "nan", "10",
                                               model, sockets, fingerprint) &&
                  benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
        report("BCL14_NONFINITE_GAIN_INVALID_ARTIFACT", passed);
        {
            static const char *const invalid_methodologies[][6] = {
                {"random", "2", "1024", "10", "5", "2"},
                {"mixed", "1", "1024", "10", "5", "2"},
                {"mixed", "2", "512", "10", "5", "2"},
                {"mixed", "2", "1024", "5", "5", "2"},
                {"mixed", "2", "1024", "10", "4", "2"},
                {"mixed", "2", "1024", "10", "5", "1"}
            };
            for (size_t index = 0; index < sizeof(invalid_methodologies) / sizeof(invalid_methodologies[0]); index++)
                passed &= write_artifact_with_methodology(artifact, nodes, source, target, "2", "100", "10", "10",
                                                          model, sockets, fingerprint,
                                                          invalid_methodologies[index][0], invalid_methodologies[index][1],
                                                          invalid_methodologies[index][2], invalid_methodologies[index][3],
                                                          invalid_methodologies[index][4], invalid_methodologies[index][5]) &&
                          benefit_calibration_load(artifact, &loaded) == BENEFIT_CALIBRATION_CONFIGURED_UNVALIDATED;
        }
        report("BCL15_NONPRODUCTION_METHODOLOGY_REJECTED", passed);
    }
    unlink(artifact); rmdir(root);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
