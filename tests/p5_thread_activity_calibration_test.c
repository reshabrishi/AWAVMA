#include "p5_thread_activity_calibration.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool passed = true;

static void check(unsigned id, const char *description, bool condition)
{
    printf("P5C2R%02u_%s: %s\n", id, description, condition ? "PASS" : "FAIL");
    passed = passed && condition;
}

static P5ThreadActivityContext context(uint32_t low, uint32_t mid, uint32_t high)
{
    P5ThreadActivityContext value = {0};

    value.schema_version = P5_THREAD_ACTIVITY_SCHEMA_VERSION;
    snprintf(value.architecture, sizeof(value.architecture), "x86_64");
    snprintf(value.cpu_vendor, sizeof(value.cpu_vendor), "FixtureVendor");
    snprintf(value.cpu_model_name, sizeof(value.cpu_model_name), "Fixture CPU");
    value.cpu_family = 6;
    value.cpu_model = 42;
    value.online_cpu_count = 8;
    value.numa_node_count = 2;
    value.page_size = 4096;
    snprintf(value.metric_name, sizeof(value.metric_name), "%s", P5_THREAD_ACTIVITY_METRIC_NAME);
    value.metric_version = P5_THREAD_ACTIVITY_METRIC_VERSION;
    snprintf(value.metric_unit, sizeof(value.metric_unit), "%s", P5_THREAD_ACTIVITY_METRIC_UNIT);
    value.worker_count = 2;
    value.memory_mb = 256;
    value.placement_mode = P5_THREAD_ACTIVITY_PLACEMENT_LOCAL;
    value.benchmark_seed = 12345;
    snprintf(value.benchmark_definition_version, sizeof(value.benchmark_definition_version), "benchmark-v1");
    snprintf(value.intensity_profile_version, sizeof(value.intensity_profile_version), "matrix-a-v1");
    value.low_intensity_percent = low;
    value.mid_intensity_percent = mid;
    value.high_intensity_percent = high;
    value.numa_balancing_state = 0;
    value.window_interval_count = P5_THREAD_ACTIVITY_WINDOW_INTERVALS;
    value.minimum_worker_run_units = P5_THREAD_ACTIVITY_MIN_WORKER_RUN_UNITS;
    (void)p5_thread_activity_hardware_fingerprint(&value, value.hardware_fingerprint,
                                                   sizeof(value.hardware_fingerprint));
    return value;
}

static uint32_t intensity(const P5ThreadActivityContext *ctx, P5ThreadActivityProfile profile)
{
    return profile == P5_THREAD_ACTIVITY_PROFILE_LOW ? ctx->low_intensity_percent :
           profile == P5_THREAD_ACTIVITY_PROFILE_MID ? ctx->mid_intensity_percent :
                                                        ctx->high_intensity_percent;
}

static P5ThreadActivityRawSample row(const P5ThreadActivityContext *ctx,
                                     P5ThreadActivityProfile profile, uint64_t run,
                                     uint32_t worker, uint64_t sample_index, double rate)
{
    P5ThreadActivityRawSample value = {0};

    value.schema_version = P5_THREAD_ACTIVITY_SCHEMA_VERSION;
    snprintf(value.calibration_id, sizeof(value.calibration_id), "fixture");
    value.run_index = run;
    value.worker_index = worker;
    value.sample_index = sample_index;
    value.controlled_profile = profile;
    value.intensity_percent = intensity(ctx, profile);
    value.sample_kind = P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL;
    value.placement_mode = ctx->placement_mode;
    value.worker_count = ctx->worker_count;
    value.memory_mb = ctx->memory_mb;
    value.interval_ms = 10;
    value.load_operations_delta = (uint64_t)(rate * 10.0);
    value.load_rate_ops_per_ms = rate;
    snprintf(value.metric_name, sizeof(value.metric_name), "%s", ctx->metric_name);
    value.metric_version = ctx->metric_version;
    snprintf(value.metric_unit, sizeof(value.metric_unit), "%s", ctx->metric_unit);
    value.valid = true;
    return value;
}

/* The optional first LOW worker/run supplies explicit window-derivation fixtures. */
static size_t dataset(const P5ThreadActivityContext *ctx, P5ThreadActivityRawSample *samples,
                      const double *first_low_rates, size_t first_low_count)
{
    size_t count = 0;

    for (unsigned profile = 0; profile < 3; profile++) {
        for (uint64_t run = 0; run < 6; run++) {
            for (uint32_t worker = 0; worker < 2; worker++) {
                size_t rates = profile == P5_THREAD_ACTIVITY_PROFILE_LOW && run == 0 && worker == 0 &&
                               first_low_rates != NULL ? first_low_count : P5_THREAD_ACTIVITY_WINDOW_INTERVALS;
                double rate = profile == P5_THREAD_ACTIVITY_PROFILE_LOW ? 10.0 :
                              profile == P5_THREAD_ACTIVITY_PROFILE_MID ? 40.0 : 100.0;
                for (size_t index = 0; index < rates; index++) {
                    double current_rate = first_low_rates != NULL && profile == P5_THREAD_ACTIVITY_PROFILE_LOW &&
                                          run == 0 && worker == 0 ? first_low_rates[index] : rate;
                    samples[count++] = row(ctx, (P5ThreadActivityProfile)profile, run, worker,
                                           (uint64_t)profile * 10000 + run * 100 + worker * 20 + index,
                                           current_rate);
                }
            }
        }
    }
    return count;
}

static int build(const P5ThreadActivityContext *ctx, P5ThreadActivityRawSample *samples,
                 size_t count, P5ThreadActivityCalibration *calibration, char *reason)
{
    return p5_thread_activity_calibration_build(ctx, "fixture", samples, count, calibration, reason, 128);
}

int main(void)
{
    P5ThreadActivityContext ctx = context(10, 40, 100), changed;
    P5ThreadActivityRawSample samples[256];
    P5ThreadActivityCalibration calibration;
    char reason[128], fingerprint[128];
    double rate;
    size_t count = dataset(&ctx, samples, NULL, 0);

    check(1, "PROFILE_ENUM_DISTINCT_FROM_CLASS_ENUM",
          strcmp(p5_thread_activity_profile_name(P5_THREAD_ACTIVITY_PROFILE_LOW), "LOW") == 0 &&
          strcmp(p5_thread_activity_class_name(P5_THREAD_ACTIVITY_COLD), "COLD") == 0);
    check(2, "LOW_PROFILE_NOT_AUTOMATICALLY_COLD",
          p5_thread_activity_classify_window_rate(NULL, 10.0) == P5_THREAD_ACTIVITY_UNAVAILABLE);
    check(3, "MATRIX_A_V1_PERCENTAGES_RECORDED", ctx.low_intensity_percent == 10 &&
          ctx.mid_intensity_percent == 40 && ctx.high_intensity_percent == 100);
    check(4, "FINGERPRINT_DETERMINISTIC", p5_thread_activity_hardware_fingerprint(&ctx, fingerprint,
          sizeof(fingerprint)) && strcmp(fingerprint, ctx.hardware_fingerprint) == 0);
    changed = ctx; changed.numa_balancing_state = 1;
    check(5, "NUMA_BALANCING_CONTEXT_STRICT", !p5_thread_activity_calibration_context_matches(
          &ctx, &changed, reason, sizeof(reason)) && strcmp(reason, "numa_balancing_state_mismatch") == 0);
    changed = ctx; changed.placement_mode = P5_THREAD_ACTIVITY_PLACEMENT_REMOTE;
    check(6, "PLACEMENT_CONTEXT_STRICT", !p5_thread_activity_calibration_context_matches(
          &ctx, &changed, reason, sizeof(reason)) && strcmp(reason, "placement_mode_mismatch") == 0);
    changed = ctx; snprintf(changed.intensity_profile_version, sizeof(changed.intensity_profile_version), "matrix-a-v2");
    check(7, "PROFILE_VERSION_CONTEXT_STRICT", !p5_thread_activity_calibration_context_matches(
          &ctx, &changed, reason, sizeof(reason)) && strcmp(reason, "intensity_profile_version_mismatch") == 0);
    changed = ctx; changed.window_interval_count = 4;
    check(8, "WINDOW_COUNT_CONTEXT_STRICT", !p5_thread_activity_calibration_context_matches(
          &ctx, &changed, reason, sizeof(reason)) && strcmp(reason, "invalid_context") == 0);
    check(9, "FIVE_INTERVAL_WINDOW_CONTRACT", ctx.window_interval_count == 5 &&
          ctx.minimum_worker_run_units == 12);
    check(10, "BASELINE_CALIBRATION_VALID", build(&ctx, samples, count, &calibration, reason) == 0 &&
          calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_VALID);
    check(11, "TWELVE_WORKER_RUN_UNITS_PER_PROFILE", calibration.low.worker_run_count == 12 &&
          calibration.mid.worker_run_count == 12 && calibration.high.worker_run_count == 12);
    check(12, "WINDOWS_ARE_NOT_FINAL_STAT_UNITS", calibration.low.window_count == 12 &&
          calibration.low.statistics.count == 12);
    check(13, "WORKER_RUN_STATISTICS_USE_MEDIANS", calibration.low.statistics.min == 10.0 &&
          calibration.low.statistics.max == 10.0 && calibration.low.statistics.median == 10.0);
    check(14, "SEPARABILITY_GATE_LOW_MID_HIGH", calibration.low.statistics.p90 < calibration.mid.statistics.p10 &&
          calibration.mid.statistics.p90 < calibration.high.statistics.p10);
    check(15, "VALID_BANDS_MAP_TO_RUNTIME_CLASSES", p5_thread_activity_classify_window_rate(&calibration, 10.0) == P5_THREAD_ACTIVITY_COLD &&
          p5_thread_activity_classify_window_rate(&calibration, 40.0) == P5_THREAD_ACTIVITY_MODERATE &&
          p5_thread_activity_classify_window_rate(&calibration, 100.0) == P5_THREAD_ACTIVITY_HOT);
    check(16, "GAP_RATIOS_ARE_REPORTED", calibration.low.gap_ratio_available && calibration.mid.gap_ratio_available);
    check(17, "WORKER_RELATIVE_DIFFERENCE_REPORTED", calibration.low.worker_agreement_count == 6);
    check(18, "LEAVE_ONE_RUN_OUT_REPORTED", calibration.low.leave_one_run_out_total == 12);
    check(19, "INTER_BAND_RATE_UNAVAILABLE", p5_thread_activity_classify_window_rate(&calibration, 20.0) == P5_THREAD_ACTIVITY_UNAVAILABLE);
    check(20, "NONFINITE_RATE_UNAVAILABLE", p5_thread_activity_classify_window_rate(&calibration, NAN) == P5_THREAD_ACTIVITY_UNAVAILABLE);
    check(21, "ZERO_INTERVAL_RATE_REJECTED", !p5_thread_activity_rate(1, 0, &rate));
    check(22, "DUPLICATE_RAW_IDENTITY_REJECTED", (samples[1].sample_index = samples[0].sample_index,
          build(&ctx, samples, count, &calibration, reason) < 0 && calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_MALFORMED));
    samples[1].sample_index++;
    check(23, "BAD_SAMPLE_SCHEMA_REJECTED", (samples[0].schema_version++,
          build(&ctx, samples, count, &calibration, reason) < 0));
    samples[0].schema_version = P5_THREAD_ACTIVITY_SCHEMA_VERSION;
    snprintf(samples[0].calibration_id, sizeof(samples[0].calibration_id), "wrong");
    check(24, "BAD_CALIBRATION_ID_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    snprintf(samples[0].calibration_id, sizeof(samples[0].calibration_id), "fixture");
    samples[0].controlled_profile = (P5ThreadActivityProfile)9;
    check(25, "BAD_PROFILE_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    samples[0].controlled_profile = P5_THREAD_ACTIVITY_PROFILE_LOW;
    samples[0].sample_kind = (P5ThreadActivitySampleKind)9;
    check(26, "BAD_SAMPLE_KIND_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    samples[0].sample_kind = P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL;
    samples[0].worker_index = ctx.worker_count;
    check(27, "WORKER_INDEX_OUT_OF_RANGE_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    samples[0].worker_index = 0;
    samples[0].interval_ms = 0;
    check(28, "ZERO_INTERVAL_SAMPLE_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    samples[0].interval_ms = 10;
    samples[0].load_rate_ops_per_ms = NAN;
    check(29, "NONFINITE_AND_INCONSISTENT_RATE_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0 &&
          (samples[0].load_rate_ops_per_ms = 99.0, build(&ctx, samples, count, &calibration, reason) < 0));
    samples[0].load_rate_ops_per_ms = 10.0;
    snprintf(samples[0].metric_name, sizeof(samples[0].metric_name), "wrong");
    check(30, "METRIC_MISMATCH_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    snprintf(samples[0].metric_name, sizeof(samples[0].metric_name), "%s", ctx.metric_name);
    samples[0].memory_mb++;
    check(31, "MEMORY_MISMATCH_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    samples[0].memory_mb = ctx.memory_mb;
    samples[0].worker_count++;
    check(32, "WORKER_COUNT_MISMATCH_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    samples[0].worker_count = ctx.worker_count;
    samples[0].placement_mode = P5_THREAD_ACTIVITY_PLACEMENT_REMOTE;
    check(33, "PLACEMENT_MISMATCH_REJECTED", build(&ctx, samples, count, &calibration, reason) < 0);
    samples[0].placement_mode = ctx.placement_mode;
    samples[0].valid = false;
    check(34, "INSUFFICIENT_SAMPLES_IS_SUCCESSFUL_BUILD", build(&ctx, samples, count, &calibration, reason) == 0 &&
          calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_INSUFFICIENT_SAMPLES);
    samples[0].valid = true;
    for (size_t index = 0; index < 15; index++) { samples[index].load_rate_ops_per_ms = 45.0; samples[index].load_operations_delta = 450; }
    check(35, "NON_SEPARABLE_IS_SUCCESSFUL_BUILD", build(&ctx, samples, count, &calibration, reason) == 0 &&
          calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_NOT_SEPARABLE);
    for (size_t index = 0; index < 15; index++) { samples[index].load_rate_ops_per_ms = 10.0; samples[index].load_operations_delta = 100; }
    samples[0].sample_kind = P5_THREAD_ACTIVITY_SAMPLE_WARMUP_RUN;
    check(36, "WARMUP_RUN_DISCARDS_WORKER_RUN", build(&ctx, samples, count, &calibration, reason) == 0 &&
          calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_INSUFFICIENT_SAMPLES);
    samples[0].sample_kind = P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL;
    samples[0].sample_kind = P5_THREAD_ACTIVITY_SAMPLE_WARMUP_INTERVAL;
    check(37, "WARMUP_INTERVAL_BREAKS_SEQUENCE", build(&ctx, samples, count, &calibration, reason) == 0 &&
          calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_INSUFFICIENT_SAMPLES);
    samples[0].sample_kind = P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL;
    samples[0].valid = false;
    check(38, "INVALID_INTERVAL_IS_SEGMENT_NOT_MALFORMED", build(&ctx, samples, count, &calibration, reason) == 0 &&
          calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_INSUFFICIENT_SAMPLES);
    samples[0].valid = true;
    changed = ctx; changed.low_intensity_percent = 0;
    check(39, "ZERO_LOW_PERCENT_INVALID", build(&changed, samples, count, &calibration, reason) < 0 &&
          calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_INVALID_CONTEXT);
    changed = ctx; changed.mid_intensity_percent = 10;
    check(40, "EQUAL_PERCENTAGES_INVALID", build(&changed, samples, count, &calibration, reason) < 0);
    changed = ctx; changed.low_intensity_percent = 40; changed.mid_intensity_percent = 10;
    check(41, "REVERSED_PERCENTAGES_INVALID", build(&changed, samples, count, &calibration, reason) < 0);
    changed = ctx; changed.high_intensity_percent = 101;
    check(42, "PERCENTAGE_ABOVE_100_INVALID", build(&changed, samples, count, &calibration, reason) < 0);
    changed = context(15, 50, 90); count = dataset(&changed, samples, NULL, 0);
    check(43, "ORDERED_CONFIGURABLE_PERCENTAGES_VALID", build(&changed, samples, count, &calibration, reason) == 0 &&
          calibration.status == P5_THREAD_ACTIVITY_CALIBRATION_VALID);
    check(44, "PROFILE_PERCENTAGES_STRICTLY_MATCHED", !p5_thread_activity_calibration_context_matches(
          &ctx, &changed, reason, sizeof(reason)) && strcmp(reason, "low_intensity_percent_mismatch") == 0);
    count = dataset(&ctx, samples, NULL, 0); (void)build(&ctx, samples, count, &calibration, reason);
    calibration.status = P5_THREAD_ACTIVITY_CALIBRATION_NOT_SEPARABLE;
    check(45, "INVALID_CALIBRATION_CANNOT_CLASSIFY", p5_thread_activity_classify_window_rate(&calibration, 10.0) == P5_THREAD_ACTIVITY_UNAVAILABLE);
    {
        const double rates[] = {8, 9, 10, 11, 12, 18, 19, 20, 21, 22, 28, 29, 30, 31, 32};
        count = dataset(&ctx, samples, rates, sizeof(rates) / sizeof(rates[0]));
        (void)build(&ctx, samples, count, &calibration, reason);
        check(46, "MULTI_WINDOW_MEDIANS_PRESERVED", calibration.low.window_count == 14);
        check(47, "WORKER_RUN_MEDIAN_OF_WINDOWS_CORRECT", fabs(calibration.low.statistics.max - 20.0) < 1e-12 &&
              fabs(calibration.low.statistics.mean - (130.0 / 12.0)) < 1e-12);
    }
    {
        const double rates[] = {8, 9, 10, 11, 12, 18, 19, 20, 21, 22, 999};
        count = dataset(&ctx, samples, rates, sizeof(rates) / sizeof(rates[0]));
        (void)build(&ctx, samples, count, &calibration, reason);
        check(48, "LEFTOVER_INTERVAL_IGNORED", calibration.low.window_count == 13 &&
              fabs(calibration.low.statistics.max - 15.0) < 1e-12);
    }
    {
        const double rates[] = {8, 9, 10, 11, 12, 99, 18, 19, 20, 21, 22};
        count = dataset(&ctx, samples, rates, sizeof(rates) / sizeof(rates[0])); samples[5].valid = false;
        (void)build(&ctx, samples, count, &calibration, reason);
        check(49, "INVALID_INTERVAL_BREAKS_SEQUENCE", calibration.low.window_count == 13 &&
              fabs(calibration.low.statistics.max - 15.0) < 1e-12);
    }
    {
        const double rates[] = {8, 9, 10, 11, 12, 18, 19, 20, 21, 22};
        count = dataset(&ctx, samples, rates, sizeof(rates) / sizeof(rates[0]));
        for (size_t index = 3; index < 10; index++) samples[index].sample_index++;
        (void)build(&ctx, samples, count, &calibration, reason);
        check(50, "SAMPLE_INDEX_GAP_BREAKS_SEQUENCE", calibration.low.window_count == 12 &&
              calibration.low.statistics.max == 18.0);
    }
    {
        const double rates[] = {1, 2, 99, 8, 9, 10, 11, 12};
        count = dataset(&ctx, samples, rates, sizeof(rates) / sizeof(rates[0])); samples[2].valid = false;
        (void)build(&ctx, samples, count, &calibration, reason);
        check(51, "INVALID_IN_PARTIAL_GROUP_CANNOT_CROSS_WINDOW", calibration.low.window_count == 12 &&
              calibration.low.statistics.max == 10.0);
    }
    {
        const double rates[] = {8, 9, 10, 11, 12, 18, 19, 20, 21, 22};
        count = dataset(&ctx, samples, rates, sizeof(rates) / sizeof(rates[0]));
        (void)build(&ctx, samples, count, &calibration, reason);
        check(52, "WINDOWS_ARE_NON_OVERLAPPING", calibration.low.window_count == 13 &&
              fabs(calibration.low.statistics.max - 15.0) < 1e-12);
    }
    return passed ? 0 : 1;
}
