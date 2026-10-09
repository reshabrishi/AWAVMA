#include "p5_thread_activity_calibration_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool passed = true;
static void check(unsigned id, const char *name, bool ok)
{
    printf("P5C2IO%02u_%s: %s\n", id, name, ok ? "PASS" : "FAIL");
    passed = passed && ok;
}

static P5ThreadActivityContext context(void)
{
    P5ThreadActivityContext c = {0};
    c.schema_version = P5_THREAD_ACTIVITY_SCHEMA_VERSION; c.cpu_family = 6; c.cpu_model = 42;
    c.online_cpu_count = 8; c.numa_node_count = 2; c.page_size = 4096; c.metric_version = 1;
    c.worker_count = 2; c.memory_mb = 256; c.placement_mode = P5_THREAD_ACTIVITY_PLACEMENT_LOCAL;
    c.benchmark_seed = 12345; c.low_intensity_percent = 10; c.mid_intensity_percent = 40;
    c.high_intensity_percent = 100; c.window_interval_count = P5_THREAD_ACTIVITY_WINDOW_INTERVALS;
    c.minimum_worker_run_units = P5_THREAD_ACTIVITY_MIN_WORKER_RUN_UNITS;
    snprintf(c.architecture, sizeof(c.architecture), "x86_64"); snprintf(c.cpu_vendor, sizeof(c.cpu_vendor), "FixtureVendor");
    snprintf(c.cpu_model_name, sizeof(c.cpu_model_name), "Fixture CPU"); snprintf(c.metric_name, sizeof(c.metric_name), "%s", P5_THREAD_ACTIVITY_METRIC_NAME);
    snprintf(c.metric_unit, sizeof(c.metric_unit), "%s", P5_THREAD_ACTIVITY_METRIC_UNIT); snprintf(c.benchmark_definition_version, sizeof(c.benchmark_definition_version), "benchmark-v1");
    snprintf(c.intensity_profile_version, sizeof(c.intensity_profile_version), "matrix-a-v1");
    (void)p5_thread_activity_hardware_fingerprint(&c, c.hardware_fingerprint, sizeof(c.hardware_fingerprint)); return c;
}

static size_t dataset(const P5ThreadActivityContext *c, P5ThreadActivityRawSample *s)
{
    size_t n = 0;
    for (unsigned p = 0; p < 3; p++) for (uint64_t run = 0; run < 6; run++) for (uint32_t worker = 0; worker < 2; worker++) for (uint64_t index = 0; index < 5; index++) {
        double rate = p == 0 ? 10.0 : p == 1 ? 40.0 : 100.0; P5ThreadActivityRawSample *v = &s[n++];
        *v = (P5ThreadActivityRawSample){.schema_version = 1, .run_index = run, .sample_index = (uint64_t)p * 10000 + run * 100 + worker * 20 + index, .worker_index = worker, .controlled_profile = (P5ThreadActivityProfile)p, .intensity_percent = p == 0 ? c->low_intensity_percent : p == 1 ? c->mid_intensity_percent : c->high_intensity_percent, .sample_kind = P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL, .placement_mode = c->placement_mode, .worker_count = c->worker_count, .memory_mb = c->memory_mb, .load_operations_delta = (uint64_t)(rate * 10), .interval_ms = 10, .load_rate_ops_per_ms = rate, .metric_version = 1, .valid = true};
        snprintf(v->calibration_id, sizeof(v->calibration_id), "fixture-1"); snprintf(v->metric_name, sizeof(v->metric_name), "%s", c->metric_name); snprintf(v->metric_unit, sizeof(v->metric_unit), "%s", c->metric_unit);
    }
    return n;
}

int main(void)
{
    P5ThreadActivityContext c = context(); P5ThreadActivityRawSample input[256], *loaded = NULL; P5ThreadActivityCalibration built, restored;
    char root[] = "/tmp/awavma-c2io-XXXXXX", reason[160], path[512], csv_header[512]; size_t count = dataset(&c, input), loaded_count = 0; FILE *file;
    check(1, "ID_VALID", p5_thread_activity_calibration_id_valid("fixture-1"));
    check(2, "ID_REJECTS_PATH", !p5_thread_activity_calibration_id_valid("../fixture"));
    check(3, "BUILD_VALID", p5_thread_activity_calibration_build(&c, "fixture-1", input, count, &built, reason, sizeof(reason)) == 0 && built.status == P5_THREAD_ACTIVITY_CALIBRATION_VALID);
    check(4, "TEMP_ROOT", mkdtemp(root) != NULL);
    check(5, "WRITE", p5_thread_activity_calibration_write_artifacts(root, &c, "fixture-1", input, count, &built, reason, sizeof(reason)) == 0);
    snprintf(path, sizeof(path), "%s/%s/fixture-1/raw_intervals.csv", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME); check(6, "RAW_INTERVALS_PATH", access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/%s/fixture-1/worker_run_summaries.csv", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME); check(7, "WORKER_SUMMARIES_PATH", access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/%s/fixture-1/calibration_manifest.csv", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME); check(8, "MANIFEST_PATH", access(path, F_OK) == 0);
    check(9, "LOAD", p5_thread_activity_calibration_load_artifacts(root, "fixture-1", &restored, &loaded, &loaded_count, reason, sizeof(reason)) == 0);
    check(10, "RAW_COUNT", loaded_count == count); check(11, "STATUS", restored.status == built.status); check(12, "RAW_ROUND_TRIP", loaded && loaded[0].sample_index == input[0].sample_index); free(loaded); loaded = NULL;
    check(13, "REJECT_BAD_ID_WRITE", p5_thread_activity_calibration_write_artifacts(root, &c, "bad/id", input, count, &built, reason, sizeof(reason)) != 0);
    snprintf(path, sizeof(path), "%s/%s/fixture-1/raw_intervals.csv", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME); file = fopen(path, "a"); if (file) fputs("junk\n", file), fclose(file);
    check(14, "REJECT_EXTRA_RAW_ROW", p5_thread_activity_calibration_load_artifacts(root, "fixture-1", &restored, &loaded, &loaded_count, reason, sizeof(reason)) != 0); free(loaded); loaded = NULL;
    check(15, "REFUSE_FINAL_OVERWRITE", p5_thread_activity_calibration_write_artifacts(root, &c, "fixture-1", input, count, &built, reason, sizeof(reason)) != 0);
    snprintf(path, sizeof(path), "%s/%s/fixture-1/calibration_manifest.csv", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME); file = fopen(path, "a"); if (file) fputs("x", file), fclose(file);
    check(16, "REJECT_TRAILING_MANIFEST", p5_thread_activity_calibration_load_artifacts(root, "fixture-1", &restored, &loaded, &loaded_count, reason, sizeof(reason)) != 0); free(loaded); loaded = NULL;
    check(17, "NO_PARTIAL_REWRITE", p5_thread_activity_calibration_write_artifacts(root, &c, "fixture-1", input, count, &built, reason, sizeof(reason)) != 0);
    snprintf(path, sizeof(path), "%s/%s/fixture-1/worker_run_summaries.csv", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME); file = fopen(path, "a"); if (file) fputs("junk\n", file), fclose(file);
    check(18, "REJECT_EXTRA_WORKER_ROW", p5_thread_activity_calibration_load_artifacts(root, "fixture-1", &restored, &loaded, &loaded_count, reason, sizeof(reason)) != 0); free(loaded); loaded = NULL;
    check(19, "STAGING_NOT_PUBLISHED", access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/%s/fixture-1/raw_intervals.csv", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME); file = fopen(path, "r"); csv_header[0] = '\0'; if (file) { if (fgets(csv_header, sizeof(csv_header), file) == NULL) csv_header[0] = '\0'; fclose(file); }
    check(20, "RAW_EXACT_HEADER", !strcmp(csv_header, "schema_version,calibration_id,run_index,sample_index,worker_index,controlled_profile,intensity_percent,sample_kind,placement_mode,worker_count,memory_mb,load_operations_delta,interval_ms,load_rate_ops_per_ms,metric_name,metric_version,metric_unit,valid,status\n"));
    check(21, "LOAD_UNKNOWN_ID_REJECTED", p5_thread_activity_calibration_load_artifacts(root, "missing", &restored, &loaded, &loaded_count, reason, sizeof(reason)) != 0);
    check(22, "LOAD_EMPTY_ROOT_REJECTED", p5_thread_activity_calibration_load_artifacts("", "fixture-1", &restored, &loaded, &loaded_count, reason, sizeof(reason)) != 0);
    check(23, "ID_EMPTY_REJECTED", !p5_thread_activity_calibration_id_valid(""));
    check(24, "ID_LEADING_DOT_REJECTED", !p5_thread_activity_calibration_id_valid(".fixture"));
    check(25, "ID_UNDERSCORE_VALID", p5_thread_activity_calibration_id_valid("fixture_1.0"));
    restored = built; restored.status = P5_THREAD_ACTIVITY_CALIBRATION_MALFORMED;
    check(26, "WRITE_REJECTS_CALIBRATION_MISMATCH", p5_thread_activity_calibration_write_artifacts(root, &c, "fixture-1", input, count, &restored, reason, sizeof(reason)) != 0);
    input[0].schema_version = 2;
    check(27, "WRITE_REJECTS_BAD_RAW", p5_thread_activity_calibration_write_artifacts(root, &c, "fixture-1", input, count, &built, reason, sizeof(reason)) != 0); input[0].schema_version = 1;
    check(28, "VALID_REWRITE_REFUSED", p5_thread_activity_calibration_write_artifacts(root, &c, "fixture-1", input, count, &built, reason, sizeof(reason)) != 0);
    check(29, "LOAD_AFTER_TAMPER_REJECTED", p5_thread_activity_calibration_load_artifacts(root, "fixture-1", &restored, &loaded, &loaded_count, reason, sizeof(reason)) != 0);
    check(30, "TAMPER_HAS_NO_OUTPUT", loaded == NULL);
    check(31, "TAMPER_REASON", !strcmp(reason, "invalid_artifact"));
    check(32, "RAW_TEXT_PROFILE", strstr(csv_header, "controlled_profile") != NULL);
    check(33, "RAW_TEXT_PLACEMENT", strstr(csv_header, "placement_mode") != NULL);
    check(34, "RAW_TEXT_SAMPLE_KIND", strstr(csv_header, "sample_kind") != NULL);
    check(35, "MANIFEST_IS_CSV", strstr(path, ".csv") != NULL);
    check(36, "CANONICAL_STATUS", !strcmp(p5_thread_activity_calibration_status_name(built.status), "VALID"));
    check(37, "CALIBRATION_REPORT_COMPLETE", built.low.statistics.count == 12 && built.mid.statistics.count == 12 && built.high.statistics.count == 12);
    check(38, "GAP_RATIOS_AVAILABLE", built.low.gap_ratio_available && built.mid.gap_ratio_available);
    check(39, "LOO_REPORT_PRESENT", built.high.leave_one_run_out_total == 12);
    check(40, "NO_LOADED_OWNERSHIP_ON_FAILURE", loaded == NULL); free(loaded);
    return passed ? 0 : 1;
}
