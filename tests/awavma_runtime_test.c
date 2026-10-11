#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void report(const char *id, int passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
}

static void report_not_evaluated(const char *id)
{
    printf("%s: NOT_EVALUATED\n", id);
}

static const char *run_stage_name(awavma_runtime_test_run_stage_t stage)
{
    switch (stage) {
    case AWAVMA_RUNTIME_TEST_RUN_STAGE_MONITOR: return "monitor";
    case AWAVMA_RUNTIME_TEST_RUN_STAGE_PIPELINE_SNAPSHOT: return "pipeline_snapshot";
    case AWAVMA_RUNTIME_TEST_RUN_STAGE_RESULTS_PUBLISH: return "runtime_results_publish";
    default: return "none";
    }
}

static const char *monitor_stage_name(runtime_monitor_test_stage_t stage)
{
    switch (stage) {
    case RUNTIME_MONITOR_TEST_STAGE_DISCOVERY_SCAN: return "discovery_scan";
    case RUNTIME_MONITOR_TEST_STAGE_DISCOVERY_SNAPSHOT: return "discovery_snapshot";
    case RUNTIME_MONITOR_TEST_STAGE_MANAGER_UPDATE: return "manager_update";
    case RUNTIME_MONITOR_TEST_STAGE_ACTIVE_SNAPSHOT: return "active_snapshot";
    case RUNTIME_MONITOR_TEST_STAGE_WORKER_WAIT: return "worker_wait";
    default: return "none";
    }
}

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool target_filter(const application_manager_record_t *application, void *context)
{
    return application->pid == *(const pid_t *)context;
}

static bool target_discovery_admission(const application_discovery_record_t *application,
                                       void *context)
{
    return application->pid == *(const pid_t *)context;
}

static int calibration_diagnostic_helpers_case(void)
{
    CalibrationRecord records[2] = {{0}};
    CalibrationSnapshot snapshot = {.records = records, .count = 2};
    CalibrationMatchRequest request = {0};
    ValidatedCalibrationMatch match;
    char canonical[512];

    snprintf(request.compatibility.cpu_architecture, sizeof(request.compatibility.cpu_architecture), "x86_64");
    snprintf(request.compatibility.cpu_model, sizeof(request.compatibility.cpu_model), "test-cpu");
    request.compatibility.online_numa_nodes = 2;
    request.compatibility.local_node = 0;
    request.compatibility.remote_node = 1;
    request.compatibility.numa_distance = 21;
    request.compatibility.local_permitted_cpu_count = 4;
    request.compatibility.page_size_bytes = 4096;
    calibration_topology_fingerprint(&request.compatibility, request.compatibility.topology_fingerprint);
    snprintf(request.workload.benchmark_pattern, sizeof(request.workload.benchmark_pattern), "hot");
    request.workload.threads = 2;
    request.workload.memory_bytes = request.workload.memory_pages = 4096;
    request.workload.duration_seconds = 30.0;
    request.action = VALIDATION_ACTION_MOVE_MEMORY;
    request.source_node = 1;
    request.destination_node = 0;
    request.migration_page_bucket = 64;
    records[0].compatibility = request.compatibility;
    records[0].compatibility.local_permitted_cpu_count = 1;
    calibration_topology_fingerprint(&records[0].compatibility, records[0].compatibility.topology_fingerprint);
    records[1].compatibility = request.compatibility;
    for (size_t index = 0; index < 2; index++) {
        snprintf(records[index].calibration_id, sizeof(records[index].calibration_id), "record-%zu", index);
        records[index].workload = request.workload;
        records[index].action = request.action;
        records[index].source_node = request.source_node;
        records[index].destination_node = request.destination_node;
        records[index].migration_page_bucket = request.migration_page_bucket;
    }
    return calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED &&
        awavma_runtime_test_calibration_result_record_index(&snapshot, &request,
                                                             CALIBRATION_MATCHED) == 1 &&
        calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED &&
        awavma_runtime_test_calibration_diagnostic_should_emit(false, CALIBRATION_MATCHED,
                                                                CALIBRATION_MATCHED) &&
        !awavma_runtime_test_calibration_diagnostic_should_emit(true, CALIBRATION_MATCHED,
                                                                 CALIBRATION_MATCHED) &&
        awavma_runtime_test_calibration_diagnostic_should_emit(true, CALIBRATION_MATCHED,
                                                                CALIBRATION_TOPOLOGY_MISMATCH) &&
        awavma_runtime_test_calibration_canonical_topology(&request.compatibility, canonical,
                                                            sizeof(canonical)) &&
        strcmp(canonical, "x86_64|test-cpu|2|0|1|21|4|4096") == 0 &&
        strstr(canonical, "0x") == NULL;
}

static int cloudlab_calibration_compatibility_case(void)
{
    int cpu_nodes[CPU_SETSIZE];
    cpu_set_t online, pinned, full, remote_only;
    calibration_compatibility_t pinned_compatibility, full_compatibility;
    CalibrationRecord record = {0};
    CalibrationSnapshot snapshot = {.records = &record, .count = 1};
    CalibrationMatchRequest request = {0};
    ValidatedCalibrationMatch match;

    CPU_ZERO(&online); CPU_ZERO(&pinned); CPU_ZERO(&full); CPU_ZERO(&remote_only);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) cpu_nodes[cpu] = -1;
    for (int cpu = 0; cpu < 64; cpu++) {
        CPU_SET(cpu, &online);
        cpu_nodes[cpu] = cpu < 32 ? 0 : 1;
        if (cpu < 32) CPU_SET(cpu, &full);
    }
    CPU_SET(0, &pinned);
    CPU_SET(32, &remote_only);
    if (!awavma_runtime_test_calibration_compatibility(
            "x86_64", "Intel(R) Xeon(R) Gold 6142 CPU @ 2.60GHz", 2, 1, 0, 21,
            4096, &online, &pinned, cpu_nodes, &pinned_compatibility) ||
        !awavma_runtime_test_calibration_compatibility(
            "x86_64", "Intel(R) Xeon(R) Gold 6142 CPU @ 2.60GHz", 2, 1, 0, 21,
            4096, &online, &full, cpu_nodes, &full_compatibility) ||
        awavma_runtime_test_calibration_compatibility(
            "x86_64", "Intel(R) Xeon(R) Gold 6142 CPU @ 2.60GHz", 2, 1, 0, 21,
            4096, &online, &remote_only, cpu_nodes, &record.compatibility))
        return 0;
    if (pinned_compatibility.local_permitted_cpu_count != 32 ||
        strcmp(pinned_compatibility.topology_fingerprint, "top1-6c79106e40370ca5") != 0 ||
        strcmp(pinned_compatibility.topology_fingerprint, full_compatibility.topology_fingerprint) != 0)
        return 0;
    record.compatibility = pinned_compatibility;
    snprintf(record.calibration_version, sizeof(record.calibration_version), "p4c-v2");
    snprintf(record.calibration_id, sizeof(record.calibration_id), "cloudlab-hot");
    snprintf(record.workload.benchmark_pattern, sizeof(record.workload.benchmark_pattern), "hot");
    record.workload.threads = 2;
    record.workload.memory_bytes = 1073741824;
    record.workload.memory_pages = 262144;
    record.workload.duration_seconds = 30.0;
    record.action = VALIDATION_ACTION_MOVE_MEMORY;
    record.source_node = 1;
    record.destination_node = 0;
    record.migration_page_bucket = 4096;
    request.compatibility = pinned_compatibility;
    request.workload = record.workload;
    request.action = record.action;
    request.source_node = record.source_node;
    request.destination_node = record.destination_node;
    request.migration_page_bucket = record.migration_page_bucket;
    if (calibration_match(&snapshot, &request, &match) != CALIBRATION_MATCHED)
        return 0;
    snprintf(request.compatibility.cpu_architecture, sizeof(request.compatibility.cpu_architecture), "aarch64");
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.compatibility = pinned_compatibility;
    snprintf(request.compatibility.cpu_model, sizeof(request.compatibility.cpu_model), "other");
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.compatibility = pinned_compatibility; request.compatibility.online_numa_nodes = 3;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.compatibility = pinned_compatibility; request.compatibility.local_node = 1;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.compatibility = pinned_compatibility; request.compatibility.remote_node = 0;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.compatibility = pinned_compatibility; request.compatibility.numa_distance = 31;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.compatibility = pinned_compatibility; request.compatibility.local_permitted_cpu_count = 31;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.compatibility = pinned_compatibility; request.compatibility.page_size_bytes = 8192;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.compatibility = pinned_compatibility;
    snprintf(request.workload.benchmark_pattern, sizeof(request.workload.benchmark_pattern), "mixed");
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.workload = record.workload; request.workload.threads = 3;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.workload = record.workload; request.workload.memory_bytes = 4096;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.workload = record.workload; request.workload.duration_seconds = 31.0;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.workload = record.workload; request.action = VALIDATION_ACTION_MOVE_THREAD;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.action = record.action; request.source_node = 0;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.source_node = record.source_node; request.destination_node = 1;
    if (calibration_match(&snapshot, &request, &match) == CALIBRATION_MATCHED) return 0;
    request.destination_node = record.destination_node; request.migration_page_bucket = 1;
    return calibration_match(&snapshot, &request, &match) != CALIBRATION_MATCHED;
}

int main(void)
{
    char root[] = "/tmp/awavma-runtime-test-XXXXXX";
    char cwd[PATH_MAX];
    char bin_dir[PATH_MAX + 32];
    char config_path[PATH_MAX + 32];
    char results_path[PATH_MAX + 32];
    char history_path[PATH_MAX + 256];
    char history[2048] = {0};
    char failure_root[] = "/tmp/awavma-temporal-failure-XXXXXX";
    char missing_bin[] = "/tmp/awavma-no-classifier";
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    awavma_runtime_record_t records[4];
    pid_t child;
    size_t count;
    int passed = 1;
    int ar01, ar02, ar03, ar04, ar05, ar06, ar07, ar08, ar09, ar10, ar11;
    int run_result;
    uint64_t run_started_ms;
    uint64_t run_elapsed_ms;
    awavma_runtime_test_run_diagnostics_t diagnostics;

    if (mkdtemp(root) == NULL || getcwd(cwd, sizeof(cwd)) == NULL) {
        report("AR01", 0);
        return EXIT_FAILURE;
    }
    child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    if (child < 0) {
        report("AR01", 0);
        return EXIT_FAILURE;
    }
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cwd);
    snprintf(config_path, sizeof(config_path), "%s/config/awavma.conf", cwd);
    awavma_runtime_config_default(&config);
    config.root_dir = root;
    config.bin_dir = bin_dir;
    config.phase_config_path = config_path;
    config.monitor_interval_ms = 25;
    config.evaluation_interval_ms = 160;
    config.max_applications = 256;
    config.worker_count = 1;
    config.queue_capacity = 4;
    config.discovery_admission = target_discovery_admission;
    config.discovery_admission_context = &child;
    config.application_filter = target_filter;
    config.application_filter_context = &child;
    runtime = awavma_runtime_create();
    ar01 = runtime != NULL && awavma_runtime_init(runtime, &config) == 0;
    report("AR01", ar01);
    run_started_ms = monotonic_ms();
    run_result = ar01 ? awavma_runtime_run_for(runtime, 220) : EINVAL;
    run_elapsed_ms = monotonic_ms() - run_started_ms;
    ar02 = ar01 && run_result == 0;
    report("AR02", ar02);
    snprintf(results_path, sizeof(results_path), "%s/runtime_results.csv", root);
    if (!ar02) {
        struct stat root_status;
        struct stat results_status;
        int results_exists = stat(results_path, &results_status) == 0;
        int results_errno = results_exists ? 0 : errno;
        int root_exists = stat(root, &root_status) == 0 && S_ISDIR(root_status.st_mode);
        int target_alive = kill(child, 0) == 0 || errno == EPERM;

        memset(&diagnostics, 0, sizeof(diagnostics));
        (void)awavma_runtime_test_run_diagnostics(runtime, &diagnostics);
        printf("AR02_DIAGNOSTIC run_result=%d stage=%s monitor_result=%d pipeline_result=%d "
               "publish_attempted=%s publish_errno=%d discovered=%s submitted=%s phase3_began=%s "
               "monitor_records=%zu runtime_records=%zu results_path=%s results_exists=%s "
               "results_errno=%d root_exists=%s target_pid=%ld target_alive=%s elapsed_ms=%llu "
               "runtime_elapsed_ms=%llu monitor_stage=%s monitor_raw_result=%d monitor_mapped_result=%d "
               "discovery_scan_result=%d discovery_scan_errno=%d discovery_count=%zu "
               "discovery_snapshot_allocated=%s manager_update_result=%d manager_active_count=%zu "
               "active_snapshot_result=%d active_count=%zu eligibility_count=%zu target_seen_in_manager=%s "
               "submission_attempted=%s submission_result=%d worker_wait_attempted=%s worker_wait_result=%d\n",
               run_result, run_stage_name(diagnostics.failing_stage), diagnostics.monitor_result,
               diagnostics.pipeline_result, diagnostics.results_publish_attempted ? "true" : "false",
               diagnostics.publish_errno, diagnostics.target_discovered ? "true" : "false",
               diagnostics.target_submitted ? "true" : "false",
               diagnostics.phase3_began ? "true" : "false", diagnostics.monitor_record_count,
               diagnostics.runtime_record_count, results_path, results_exists ? "true" : "false",
               results_errno, root_exists ? "true" : "false", (long)child,
               target_alive ? "true" : "false", (unsigned long long)run_elapsed_ms,
               (unsigned long long)diagnostics.elapsed_ms,
               monitor_stage_name(diagnostics.monitor.failing_stage), diagnostics.monitor.raw_result,
               diagnostics.monitor.mapped_result, diagnostics.monitor.discovery_scan_result,
               diagnostics.monitor.discovery_scan_errno, diagnostics.monitor.discovery_count,
               diagnostics.monitor.discovery_snapshot_allocated ? "true" : "false",
               diagnostics.monitor.manager_update_result, diagnostics.monitor.manager_active_count,
               diagnostics.monitor.active_snapshot_result, diagnostics.monitor.active_count,
               diagnostics.monitor.eligibility_count,
               diagnostics.monitor.target_seen_in_manager ? "true" : "false",
               diagnostics.monitor.submission_attempted ? "true" : "false",
               diagnostics.monitor.submission_result,
               diagnostics.monitor.worker_wait_attempted ? "true" : "false",
               diagnostics.monitor.worker_wait_result);
        ar03 = ar04 = ar05 = ar06 = ar07 = ar08 = 0;
        report_not_evaluated("AR03");
        report_not_evaluated("AR04");
        report_not_evaluated("AR05");
        report_not_evaluated("AR06");
        report_not_evaluated("AR07");
        report_not_evaluated("AR08");
    } else {
        count = awavma_runtime_snapshot(runtime, records, 4);
        ar03 = count == 1 && records[0].pid == child && records[0].start_time_ticks != 0;
        report("AR03", ar03);
        ar04 = ar03 && records[0].phase3_samples > 0;
        report("AR04", ar04);
        ar05 = ar03 && records[0].status == AWAVMA_RUNTIME_INSUFFICIENT;
        report("AR05", ar05);
        ar06 = ar03 && strstr(records[0].detail, "unavailable") != NULL;
        report("AR06", ar06);
        ar07 = access(results_path, R_OK) == 0;
        report("AR07", ar07);
        snprintf(history_path, sizeof(history_path), "%s/apps/%s/history/thread_confidence.csv", root,
                 records[0].app_id);
        FILE *history_file = fopen(history_path, "r");
        ar08 = ar03 && records[0].temporal_generation >= 1 && records[0].temporal_history_available &&
                history_file != NULL && fread(history, 1, sizeof(history) - 1, history_file) > 0 &&
                fclose(history_file) == 0 &&
                (strstr(history, ",NA,false,false,") != NULL || strstr(history, ",true,true,") != NULL) &&
                strstr(history, "0x") == NULL && strstr(history, "address") == NULL;
        report("AR08", ar08);
    }
    awavma_runtime_destroy(runtime);
    runtime = NULL;
    passed = passed && mkdtemp(failure_root) != NULL;
    awavma_runtime_config_default(&config);
    config.root_dir = failure_root;
    config.bin_dir = missing_bin;
    config.phase_config_path = config_path;
    config.phase4_mode = AWAVMA_PHASE4_SUBPROCESS;
    config.monitor_interval_ms = 25;
    config.evaluation_interval_ms = 160;
    config.max_applications = 256;
    config.worker_count = 1;
    config.queue_capacity = 4;
    config.discovery_admission = target_discovery_admission;
    config.discovery_admission_context = &child;
    config.application_filter = target_filter;
    config.application_filter_context = &child;
    runtime = awavma_runtime_create();
    int failure_init = runtime != NULL && awavma_runtime_init(runtime, &config) == 0;
    int failure_run = failure_init ? awavma_runtime_run_for(runtime, 220) : EINVAL;
    count = runtime == NULL ? 0 : awavma_runtime_snapshot(runtime, records, 4);
    ar09 = failure_run == 0 && count == 1 && records[0].generation == 0 &&
           records[0].temporal_generation >= 1 && records[0].temporal_history_available;
    report("AR09", ar09);
    if (!ar09) {
        memset(&diagnostics, 0, sizeof(diagnostics));
        if (failure_init)
            (void)awavma_runtime_test_run_diagnostics(runtime, &diagnostics);
        printf("AR09_DIAGNOSTIC init=%s run_result=%d records=%zu monitor_stage=%s "
               "monitor_raw_result=%d discovery_scan_result=%d discovery_scan_errno=%d "
               "manager_update_result=%d worker_wait_result=%d\n",
               failure_init ? "true" : "false", failure_run, count,
               monitor_stage_name(diagnostics.monitor.failing_stage), diagnostics.monitor.raw_result,
               diagnostics.monitor.discovery_scan_result, diagnostics.monitor.discovery_scan_errno,
               diagnostics.monitor.manager_update_result, diagnostics.monitor.worker_wait_result);
    }
    awavma_runtime_destroy(runtime);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    ar10 = calibration_diagnostic_helpers_case();
    report("AR10", ar10);
    ar11 = cloudlab_calibration_compatibility_case();
    report("AR11", ar11);
    passed = ar01 && ar02 && ar03 && ar04 && ar05 && ar06 && ar07 && ar08 && ar09 && ar10 && ar11;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
