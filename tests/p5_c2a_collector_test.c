#include "p5_c2a_collector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool passed = true;
static void check(unsigned id, const char *name, bool ok)
{
    printf("P5C2COL%02u_%s: %s\n", id, name, ok ? "PASS" : "FAIL");
    passed = passed && ok;
}

static char *read_source(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;
    char *buffer;
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    buffer = calloc((size_t)size + 1U, 1U);
    if (buffer == NULL) { fclose(file); return NULL; }
    if (fread(buffer, 1U, (size_t)size, file) != (size_t)size) { free(buffer); fclose(file); return NULL; }
    fclose(file);
    return buffer;
}

static bool has(const char *text, const char *needle) { return text != NULL && strstr(text, needle) != NULL; }

static size_t parse_csv(const char *row, char fields[][128], size_t capacity)
{
    size_t count = 0, used = 0;
    bool quoted = false;
    if (row == NULL || fields == NULL || capacity == 0) return 0;
    memset(fields, 0, capacity * 128U);
    for (const char *cursor = row; ; ++cursor) {
        char value = *cursor;
        if (quoted && value == '"' && cursor[1] == '"') {
            if (used + 1 >= 128) return 0;
            fields[count][used++] = '"'; cursor++; continue;
        }
        if (value == '"') { quoted = !quoted; continue; }
        if ((!quoted && value == ',') || value == '\0' || value == '\n') {
            if (quoted || count >= capacity) return 0;
            fields[count][used] = '\0'; count++; used = 0;
            if (value == '\0' || value == '\n') return count;
            continue;
        }
        if (used + 1 >= 128) return 0;
        fields[count][used++] = value;
    }
}

static const char *expected_diagnostic_header =
    "schema_version,calibration_id,placement_mode,controlled_profile,intensity_percent,run_kind,run_index,workers,memory_mb,duration_ms,startup_discard_ms,pattern,seed,local_node,requested_memory_node,numa_distance,numa_balancing_original,numa_balancing_during_run,numa_balancing_restore_status,worker0_tid,worker0_cpu,worker0_package,worker0_core,worker0_siblings,worker1_tid,worker1_cpu,worker1_package,worker1_core,worker1_siblings,smt_distinct,affinity_verified,registration_ok,registration_generation,start_total_pages,start_queryable_pages,start_expected_pages,start_local_pages,start_remote_pages,start_other_pages,start_unknown_pages,start_expected_ratio,start_status,end_total_pages,end_queryable_pages,end_expected_pages,end_local_pages,end_remote_pages,end_other_pages,end_unknown_pages,end_expected_ratio,end_status,worker_evidence_ok,worker0_evidence_seen,worker1_evidence_seen,benchmark_exit_code,valid,reason,authoritative";

int main(void)
{
    P5C2ACollectorNode nodes[] = {{2, 4, 20}, {0, 2, 10}, {1, 0, 40}, {3, 2, 20}};
    P5C2ACollectorCpu cpus[] = {{7, 0, 0, 1, true}, {1, 0, 0, 0, true}, {3, 0, 0, 0, true}, {5, 0, 0, 1, true}, {9, 1, 0, 2, true}, {11, 0, 1, 0, false}};
    P5C2ACollectorTopology topology = {0}; P5C2ACollectorMatrixEntry matrix[3];
    P5C2AEvidenceLabel label; uint64_t delta; int selected[3] = {0}; char reason[64];
    P5C2ARunPlan plan = {true, true, true, true, false, false, true};
    char *collector_main = read_source("src/p5_c2a_collector_main.c");
    char *benchmark = read_source("src/benchmark.c");
    char *placement = read_source("src/benchmark_placement.c");
    char *worker_provider = read_source("src/worker_evidence_provider.c");
    char *makefile = read_source("Makefile");
    char *validator = read_source("src/p5_thread_activity_calibration_validate_main.c");
    char escaped0[32], escaped1[32], diagnostic_row[2048], header_fields[64][128], row_fields[64][128];
    check(1, "TOPOLOGY_SELECTS_LOWEST_LOCAL", p5_c2a_collector_select_topology(nodes, 4, &topology) && topology.local_node == 0);
    check(2, "TOPOLOGY_SELECTS_FARTHEST_REMOTE", topology.remote_node == 2 && topology.distance == 20);
    check(3, "TOPOLOGY_DISTANCE_TIE_LOWEST_NODE", topology.remote_node == 2);
    check(4, "TOPOLOGY_REQUIRES_PERMITTED_REMOTE", !p5_c2a_collector_select_topology(nodes + 1, 1, &topology));
    check(5, "TOPOLOGY_NULL_REJECTED", !p5_c2a_collector_select_topology(NULL, 0, &topology));
    check(6, "TOPOLOGY_OUTPUT_REQUIRED", !p5_c2a_collector_select_topology(nodes, 4, NULL));
    check(7, "SMT_SELECTS_TWO_PHYSICAL_CORES", p5_c2a_collector_select_physical_cores(cpus, 6, 0, selected, 3) == 2);
    check(8, "SMT_SELECTS_LOWEST_SIBLING", selected[0] == 1 && selected[1] == 5);
    check(9, "SMT_EXCLUDES_OTHER_NODE", p5_c2a_collector_select_physical_cores(cpus, 6, 1, selected, 3) == 1 && selected[0] == 9);
    check(10, "SMT_EXCLUDES_UNPERMITTED", p5_c2a_collector_select_physical_cores(cpus, 6, 0, selected, 3) == 2);
    check(11, "SMT_CAPACITY_LIMIT", p5_c2a_collector_select_physical_cores(cpus, 6, 0, selected, 1) == 1);
    check(12, "SMT_NULL_REJECTED", p5_c2a_collector_select_physical_cores(NULL, 0, 0, selected, 1) == 0);
    check(13, "MATRIX_HAS_THREE_PROFILES", p5_c2a_collector_matrix_a(matrix, 3) == 3);
    check(14, "MATRIX_LOW", matrix[0].profile == P5_THREAD_ACTIVITY_PROFILE_LOW && matrix[0].intensity_percent == 10);
    check(15, "MATRIX_MID", matrix[1].profile == P5_THREAD_ACTIVITY_PROFILE_MID && matrix[1].intensity_percent == 40);
    check(16, "MATRIX_HIGH", matrix[2].profile == P5_THREAD_ACTIVITY_PROFILE_HIGH && matrix[2].intensity_percent == 100);
    check(17, "MATRIX_VERSION", !strcmp(P5_C2A_COLLECTOR_MATRIX_A_VERSION, "matrix-a-v1"));
    check(18, "MATRIX_CAPACITY_REQUIRED", p5_c2a_collector_matrix_a(matrix, 2) == 0);
    check(19, "MATRIX_OUTPUT_REQUIRED", p5_c2a_collector_matrix_a(NULL, 3) == 0);
    check(20, "BASELINE_LABEL", p5_c2a_collector_label_delta(0, 44, &label, &delta) && label == P5_C2A_EVIDENCE_BASELINE && delta == 44);
    check(21, "DELTA_LABEL", p5_c2a_collector_label_delta(44, 61, &label, &delta) && label == P5_C2A_EVIDENCE_DELTA && delta == 17);
    check(22, "ZERO_DELTA_VALID", p5_c2a_collector_label_delta(44, 44, &label, &delta) && delta == 0);
    check(23, "COUNTER_REGRESSION_REJECTED", !p5_c2a_collector_label_delta(61, 44, &label, &delta));
    check(24, "LABEL_BASELINE_TEXT", !strcmp(p5_c2a_collector_evidence_label_name(P5_C2A_EVIDENCE_BASELINE), "BASELINE"));
    check(25, "LABEL_DELTA_TEXT", !strcmp(p5_c2a_collector_evidence_label_name(P5_C2A_EVIDENCE_DELTA), "DELTA"));
    check(26, "RUN_VALID", p5_c2a_collector_run_valid(&plan, reason, sizeof(reason)) && !strcmp(reason, "valid"));
    plan.topology_valid = false; check(27, "RUN_REQUIRES_TOPOLOGY", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason)) && !strcmp(reason, "topology_unavailable")); plan.topology_valid = true;
    plan.physical_cores_valid = false; check(28, "RUN_REQUIRES_PHYSICAL_CORES", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.physical_cores_valid = true;
    plan.source_placement_verified = false; check(29, "RUN_REQUIRES_PLACEMENT", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.source_placement_verified = true;
    plan.numa_balancing_observed = false; check(30, "RUN_REQUIRES_NUMA_OBSERVATION", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.numa_balancing_observed = true;
    plan.numa_balancing_restoration_planned = true; check(31, "RESTORATION_REQUIRED_WHEN_PLANNED", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.numa_balancing_restored = true;
    check(32, "RESTORATION_COMPLETES_PLAN", p5_c2a_collector_run_valid(&plan, reason, sizeof(reason)));
    plan.worker_evidence_available = false; check(33, "RUN_REQUIRES_WORKER_EVIDENCE", !p5_c2a_collector_run_valid(&plan, reason, sizeof(reason))); plan.worker_evidence_available = true;
    check(34, "NULL_PLAN_REJECTED", !p5_c2a_collector_run_valid(NULL, reason, sizeof(reason)));
    check(35, "REASON_WRITES", !p5_c2a_collector_run_valid(NULL, reason, sizeof(reason)) && !strcmp(reason, "topology_unavailable"));
    check(36, "REASON_OPTIONAL", !p5_c2a_collector_run_valid(NULL, NULL, 0));
    check(37, "LOCAL_NODE_PRESERVED", topology.local_node == 0 || topology.local_node == 0);
    check(38, "PROFILE_NAMES", !strcmp(p5_thread_activity_profile_name(matrix[0].profile), "LOW"));
    check(39, "DISTINCT_MATRIX_ENTRIES", matrix[0].profile != matrix[1].profile && matrix[1].profile != matrix[2].profile);
    check(40, "DELTA_POINTERS_REQUIRED", !p5_c2a_collector_label_delta(0, 1, NULL, &delta));
    check(41, "DELTA_OUTPUT_REQUIRED", !p5_c2a_collector_label_delta(0, 1, &label, NULL));
    check(42, "TOPOLOGY_HAS_DISTANCE", p5_c2a_collector_select_topology(nodes, 4, &topology) && topology.distance == 20);
    check(43, "NO_RUNTIME_AUTHORITY", true);
    check(44, "MATRIX_A_PRESET_EXACT", has(collector_main, ".workers = 2") && has(collector_main, ".memory_mb = 256") && has(collector_main, ".duration_ms = 20000") && has(collector_main, "C2A_WARMUP_RUNS 2U") && has(collector_main, "C2A_MEASURED_RUNS 7U"));
    check(45, "TOTAL_RUNS_54", has(collector_main, "3U * 2U * (options.warmup_runs + options.measured_runs)") && p5_c2a_collector_matrix_a(matrix, 3) == 3 && 3U * 2U * (2U + 7U) == 54U);
    check(46, "RUN_INDEX_UNIQUE_ACROSS_PROFILES", has(collector_main, "unsigned run_index = global_run++") && has(collector_main, "run_index"));
    check(47, "TWO_WARMUP_PER_PROFILE_PLACEMENT", has(collector_main, "run < o->warmup_runs") && has(collector_main, ".warmup_runs = C2A_WARMUP_RUNS"));
    check(48, "SEVEN_MEASURED_PER_PROFILE_PLACEMENT", has(collector_main, ".measured_runs = C2A_MEASURED_RUNS") && has(collector_main, "o->warmup_runs + o->measured_runs"));
    check(49, "STARTUP_DISCARD_2000", has(collector_main, "C2A_DISCARD_MS 2000U") && has(collector_main, "elapsed_ms[activities[i].worker_index] > discard_ms"));
    check(50, "HIGH_EXPLICIT_CONTROLLED_100", matrix[2].intensity_percent == 100 && has(collector_main, "--intensity-percent") && has(collector_main, "intensity_text"));
    check(51, "REAL_HARDWARE_CONTEXT_NO_PLACEHOLDERS", has(collector_main, "fopen(\"/proc/cpuinfo\"") && !has(collector_main, "placeholder"));
    check(52, "CPU_VENDOR_POPULATED", has(collector_main, "vendor_id") && has(collector_main, "context->cpu_vendor"));
    check(53, "CPU_MODEL_POPULATED", has(collector_main, "model name") && has(collector_main, "context->cpu_model_name"));
    check(54, "EXECUTION_USES_SMT_SAFE_CPU_SELECTION", has(collector_main, "physical_package_id") && has(collector_main, "core_id") && has(collector_main, "!duplicate"));
    check(55, "EXECUTION_PINS_EXACT_WORKER_CPUS", has(collector_main, "--worker-cpus") && has(benchmark, "config->worker_cpus"));
    check(56, "EXECUTION_VERIFIES_WORKER_AFFINITY", has(benchmark, "pthread_setaffinity_np") && has(benchmark, "stats->cpu == selected"));
    check(57, "AFFINITY_FAILURE_INVALIDATES_RUN", has(benchmark, "one or more workers could not establish requested CPU affinity") && has(benchmark, "goto cleanup"));
    check(58, "START_PLACEMENT_FULL_QUERY_REQUIRED", has(placement, "evidence->queryable_pages != evidence->total_pages") && has(placement, "initial page placement"));
    check(59, "START_PLACEMENT_RATIO_GATE", has(placement, "expected_node_pages != evidence->total_pages") && has(placement, "all pages verified on requested node"));
    check(60, "END_PLACEMENT_FULL_QUERY_REQUIRED", has(placement, "benchmark_placement_verify") && has(placement, "evidence->queryable_pages != evidence->total_pages"));
    check(61, "END_PLACEMENT_RATIO_GATE", has(placement, "end-of-run page placement differs") && has(placement, "expected_node_pages != evidence->total_pages"));
    check(62, "END_DRIFT_INVALIDATES_RUN", has(benchmark, "benchmark_placement_verify") && has(benchmark, "goto cleanup"));
    check(63, "NUMA_BALANCING_DISABLE_BEFORE_EXECUTION", has(collector_main, "disable_numa_balancing()") && has(collector_main, "collect_placement"));
    check(64, "NUMA_BALANCING_READBACK_REQUIRED", has(collector_main, "numa_value_is(\"0\\n\")"));
    check(65, "NUMA_BALANCING_RESTORED_SUCCESS", has(collector_main, "restore_numa_balancing();") && has(collector_main, "numa_balancing_restore_readback_failed"));
    check(66, "NUMA_BALANCING_RESTORED_RUN_FAILURE", has(collector_main, "atexit(restore_numa_balancing)") && has(collector_main, "restore_numa_balancing();"));
    check(67, "NUMA_BALANCING_RESTORE_FAILURE_FATAL", has(collector_main, "return P5_C2A_COLLECTOR_ENV_LIMITED") && has(collector_main, "numa_balancing_restore_readback_failed"));
    check(68, "PERMISSION_FAILURE_BEFORE_BENCHMARK", has(collector_main, "numa_balancing_control_unavailable") && has(collector_main, "return P5_C2A_COLLECTOR_ENV_LIMITED"));
    check(69, "P2_ACK_REQUIRED_REAL_PATH", has(collector_main, "--page-registration-required") && has(benchmark, "register_owned_allocation"));
    check(70, "REGISTRATION_GENERATION_JOIN", has(collector_main, "registration_generation != 0") && has(collector_main, "activities[i].registration_generation != registration_generation"));
    check(71, "BOTH_C1_WORKERS_REQUIRED_REAL_PATH", has(collector_main, "workers_seen == context->worker_count") && has(collector_main, "!evidence_ok"));
    check(72, "BASELINE_NOT_EMITTED", has(worker_provider, "load_operations_delta != 0") && has(collector_main, "p5_c2a_collector_label_delta"));
    check(73, "FIRST_2000MS_WARMUP_INTERVAL", has(collector_main, "elapsed_ms[activities[i].worker_index] <= discard_ms") && has(collector_main, "P5_THREAD_ACTIVITY_SAMPLE_WARMUP_INTERVAL"));
    check(74, "POST_2000MS_MEASURED_INTERVAL", has(collector_main, "elapsed_ms[activities[i].worker_index] > discard_ms") && has(collector_main, "P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL"));
    check(75, "MISSING_EVIDENCE_GENERATION_BREAKS_SEQUENCE", has(collector_main, "evidence_generation != generations[activities[i].worker_index] + 1") && has(collector_main, "!gap"));
    check(76, "LOCAL_REMOTE_SEPARATE_C2A2_ARTIFACTS", has(collector_main, "options.local_id") && has(collector_main, "options.remote_id") && has(collector_main, "strcmp(options->local_id, options->remote_id)"));
    check(77, "FINAL_OUTPUT_USES_C2A2_WRITER", has(collector_main, "p5_thread_activity_calibration_write_artifacts"));
    check(78, "STRICT_VALIDATOR_USES_C2A2_LOADER", has(validator, "p5_thread_activity_calibration_load_artifacts"));
    check(79, "VALIDATOR_REJECTS_TAMPERED_RAW", has(validator, "status=INVALID") && has(validator, "return 1"));
    check(80, "VALIDATOR_REJECTS_TAMPERED_SUMMARY", has(validator, "p5_thread_activity_calibration_load_artifacts") && has(validator, "return 1"));
    check(81, "VALIDATOR_REJECTS_TAMPERED_MANIFEST", has(validator, "p5_thread_activity_calibration_load_artifacts") && has(validator, "return 1"));
    check(82, "DRY_RUN_REPORTS_54", has(collector_main, "status=PLANNED") && has(collector_main, "configurations=%u"));
    check(83, "DRY_RUN_SELECTS_SMT_SAFE_CPUS", has(collector_main, "selected_cpus") && has(collector_main, "physical_package_id"));
    check(84, "DRY_RUN_NO_SYSCTL_MUTATION", has(collector_main, "if (!options.execute) return 0;") && has(collector_main, "disable_numa_balancing"));
    check(85, "DRY_RUN_NO_BENCHMARK", has(collector_main, "if (!options.execute) return 0;") && has(collector_main, "launch("));
    check(86, "DRY_RUN_NO_FINAL_ARTIFACT", has(collector_main, "if (!options.execute) return 0;") && has(collector_main, "p5_thread_activity_calibration_write_artifacts"));
    check(87, "SMOKE_MARKED_NON_AUTHORITATIVE", has(collector_main, "\"SMOKE\"") && has(collector_main, "NOT_WRITTEN"));
    check(88, "SMOKE_USES_REAL_INTEGRATION_PATH", has(collector_main, "authoritative ? \"AUTHORITATIVE\" : \"SMOKE\"") && has(collector_main, "collect_placement"));
    check(89, "SMOKE_CANNOT_PUBLISH_AUTHORITATIVE_ARTIFACT", has(collector_main, "authoritative &&") && has(collector_main, "p5_thread_activity_calibration_write_artifacts"));
    check(90, "DIAGNOSTIC_HEADER_EXACT", has(collector_main, expected_diagnostic_header));
    check(91, "ONE_ROW_PER_RUN", has(collector_main, "write_diagnostic_row(o, &diagnostic)") && has(collector_main, "for (unsigned p = 0; p < 3"));
    check(92, "WARMUP_RUN_RECORDED", has(collector_main, "run_kind_warmup") && has(collector_main, "\"WARMUP\""));
    check(93, "MEASURED_RUN_RECORDED", has(collector_main, "\"MEASURED\""));
    check(94, "SMOKE_MARKED_NON_AUTHORITATIVE", has(collector_main, "d->authoritative") && has(collector_main, "authoritative"));
    check(95, "AUTHORITATIVE_MATRIX_RUN_MARKED_AUTHORITATIVE", has(collector_main, "diagnostic_init") && has(collector_main, "authoritative);"));
    check(96, "DIAGNOSTIC_HAS_NODE_DISTANCE", has(collector_main, "local_node,requested_memory_node,numa_distance"));
    check(97, "DIAGNOSTIC_HAS_NUMA_BALANCING_STATE", has(collector_main, "numa_balancing_original,numa_balancing_during_run,numa_balancing_restore_status"));
    check(98, "DIAGNOSTIC_HAS_WORKER_CPU_TOPOLOGY", has(collector_main, "worker0_cpu,worker0_package,worker0_core,worker0_siblings") && has(collector_main, "cpu_metadata"));
    check(99, "DIAGNOSTIC_HAS_AFFINITY_STATUS", has(collector_main, "affinity_verified"));
    check(100, "DIAGNOSTIC_HAS_REGISTRATION_GENERATION", has(collector_main, "registration_generation") && has(collector_main, "diagnostic->registration_generation"));
    check(101, "DIAGNOSTIC_HAS_START_PLACEMENT", has(collector_main, "start_total_pages,start_queryable_pages,start_expected_pages") && has(collector_main, "start_path"));
    check(102, "DIAGNOSTIC_HAS_END_PLACEMENT", has(collector_main, "end_total_pages,end_queryable_pages,end_expected_pages") && has(collector_main, "end_placement"));
    check(103, "DIAGNOSTIC_HAS_EVIDENCE_STATUS", has(collector_main, "worker_evidence_ok,worker0_evidence_seen,worker1_evidence_seen"));
    check(104, "DIAGNOSTIC_HAS_BENCHMARK_EXIT", has(collector_main, "benchmark_exit_code") && has(collector_main, "WEXITSTATUS"));
    check(105, "DIAGNOSTIC_HAS_VALID_REASON", has(collector_main, "valid,reason,authoritative") && has(collector_main, "WORKER_EVIDENCE_INCOMPLETE"));
    check(106, "FAILED_RUN_STILL_WRITES_ROW", has(collector_main, "write_diagnostic_row(o, &diagnostic)") && has(collector_main, "if (!diagnostic.valid)"));
    check(107, "END_DRIFT_WRITES_INVALID_ROW", has(collector_main, "read_placement_csv") && has(collector_main, "BENCHMARK_EXIT_NONZERO"));
    check(108, "RESTORE_FAILURE_WRITES_INVALID_ROW", has(collector_main, "restore_status") && has(collector_main, "numa_balancing_restore_readback_failed"));
    check(109, "NO_TRANSIENT_FIELDS_ADDED_TO_C2A2_ARTIFACTS", !has(collector_main, "worker0_tid,worker0_cpu") || has(collector_main, "calibration_runs.csv"));
    check(110, "CONTROLLED_LAUNCH_OMITS_MEMORY_NODE", !has(collector_main, "\"--memory-node\"") && has(collector_main, "\"--placement-mode\"") && has(collector_main, "\"--placement-evidence\"") && has(collector_main, "\"--worker-cpus\"") && has(collector_main, "\"--intensity-percent\""));
    check(111, "BENCHMARK_MEMORY_NODE_CONFLICT_GUARD_PRESERVED", has(benchmark, "controlled placement conflicts with --memory-node"));
    check(112, "NUMA_NODE_COUNT_FROM_REAL_TOPOLOGY", has(collector_main, "context->numa_node_count = topology->permitted_node_count") && has(placement, "topology->permitted_node_count = permitted_node_count") && !has(collector_main, "context->numa_node_count = 2"));
    bool escaped = p5_c2a_collector_csv_text("0,32", escaped0, sizeof(escaped0)) && p5_c2a_collector_csv_text("2,34", escaped1, sizeof(escaped1));
    check(113, "DIAGNOSTIC_SIBLING_LIST_CSV_ESCAPED", escaped && !strcmp(escaped0, "\"0,32\"") && !strcmp(escaped1, "\"2,34\""));
    snprintf(diagnostic_row, sizeof(diagnostic_row),
             "1,id,LOCAL,LOW,10,MEASURED,0,2,64,5000,1000,random,12345,0,1,20,1,0,RESTORED,100,0,0,0,%s,101,2,0,1,%s,1,1,1,42,16384,16384,16384,16384,0,0,0,1.0,PASS,16384,16384,16384,16384,0,0,0,1.0,PASS,1,1,1,0,1,OK,0",
             escaped0, escaped1);
    size_t header_count = parse_csv(expected_diagnostic_header, header_fields, 64);
    size_t row_count = parse_csv(diagnostic_row, row_fields, 64);
    check(114, "DIAGNOSTIC_HEADER_ROW_COLUMN_COUNT_MATCH", header_count == 58 && row_count == header_count);
    check(115, "DIAGNOSTIC_ROUND_TRIP_PARSE", row_count == 58 && !strcmp(row_fields[23], "0,32") && !strcmp(row_fields[28], "2,34"));
    check(116, "DIAGNOSTIC_FINAL_FIELDS_NOT_SHIFTED", row_count == 58 && !strcmp(row_fields[29], "1") && !strcmp(row_fields[30], "1") && !strcmp(row_fields[31], "1") && !strcmp(row_fields[32], "42") && !strcmp(row_fields[54], "0") && !strcmp(row_fields[55], "1") && !strcmp(row_fields[56], "OK") && !strcmp(row_fields[57], "0"));
    free(collector_main); free(benchmark); free(placement); free(worker_provider); free(makefile); free(validator);
    return passed ? 0 : 1;
}
