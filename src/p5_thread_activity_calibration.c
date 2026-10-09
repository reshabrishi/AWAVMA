#include "p5_thread_activity_calibration.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_reason(char *out, size_t size, const char *value) { if (out && size) snprintf(out, size, "%s", value); }
static bool text(const char *value) { return value && value[0]; }
static bool same(const char *left, const char *right) { return text(left) && text(right) && strcmp(left, right) == 0; }
static bool placement(P5ThreadActivityPlacementMode value) { return value == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL || value == P5_THREAD_ACTIVITY_PLACEMENT_REMOTE; }
static bool profile(P5ThreadActivityProfile value) { return value >= P5_THREAD_ACTIVITY_PROFILE_LOW && value <= P5_THREAD_ACTIVITY_PROFILE_HIGH; }
static int compare_double(const void *left, const void *right) { double a = *(const double *)left, b = *(const double *)right; return a < b ? -1 : a > b; }
static int compare_sample(const void *left, const void *right) { const P5ThreadActivityRawSample *a = left, *b = right; return a->sample_index < b->sample_index ? -1 : a->sample_index > b->sample_index; }
static double percentile(const double *values, size_t count, double probability) { double rank = probability * (double)(count - 1); size_t lower = (size_t)floor(rank), upper = (size_t)ceil(rank); return values[lower] + (rank - lower) * (values[upper] - values[lower]); }
static uint32_t intensity(const P5ThreadActivityContext *context, P5ThreadActivityProfile value) { return value == P5_THREAD_ACTIVITY_PROFILE_LOW ? context->low_intensity_percent : value == P5_THREAD_ACTIVITY_PROFILE_MID ? context->mid_intensity_percent : context->high_intensity_percent; }
static P5ThreadActivityProfileReport *report(P5ThreadActivityCalibration *calibration, P5ThreadActivityProfile value) { return value == P5_THREAD_ACTIVITY_PROFILE_LOW ? &calibration->low : value == P5_THREAD_ACTIVITY_PROFILE_MID ? &calibration->mid : &calibration->high; }
static bool same_group(const P5ThreadActivityRawSample *left, const P5ThreadActivityRawSample *right) { return left->run_index == right->run_index && left->worker_index == right->worker_index && left->controlled_profile == right->controlled_profile; }

static bool context_shape(const P5ThreadActivityContext *c) {
    return c && c->schema_version == P5_THREAD_ACTIVITY_SCHEMA_VERSION && text(c->architecture) && text(c->cpu_vendor) && text(c->cpu_model_name) && c->online_cpu_count && c->numa_node_count && c->page_size && text(c->hardware_fingerprint) && same(c->metric_name, P5_THREAD_ACTIVITY_METRIC_NAME) && c->metric_version == P5_THREAD_ACTIVITY_METRIC_VERSION && same(c->metric_unit, P5_THREAD_ACTIVITY_METRIC_UNIT) && c->worker_count && c->memory_mb && placement(c->placement_mode) && text(c->benchmark_definition_version) && text(c->intensity_profile_version) && c->low_intensity_percent >= 1 && c->low_intensity_percent < c->mid_intensity_percent && c->mid_intensity_percent < c->high_intensity_percent && c->high_intensity_percent <= 100 && c->window_interval_count == P5_THREAD_ACTIVITY_WINDOW_INTERVALS && c->minimum_worker_run_units == P5_THREAD_ACTIVITY_MIN_WORKER_RUN_UNITS;
}
static bool valid_row(const P5ThreadActivityContext *c, const char *id, const P5ThreadActivityRawSample *s) {
    double rate;
    return s->schema_version == P5_THREAD_ACTIVITY_SCHEMA_VERSION && same(s->calibration_id, id) && profile(s->controlled_profile) && s->sample_kind >= P5_THREAD_ACTIVITY_SAMPLE_WARMUP_RUN && s->sample_kind <= P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL && s->worker_index < c->worker_count && s->intensity_percent == intensity(c, s->controlled_profile) && s->placement_mode == c->placement_mode && s->worker_count == c->worker_count && s->memory_mb == c->memory_mb && same(s->metric_name, c->metric_name) && s->metric_version == c->metric_version && same(s->metric_unit, c->metric_unit) && s->interval_ms && isfinite(s->load_rate_ops_per_ms) && p5_thread_activity_rate(s->load_operations_delta, s->interval_ms, &rate) && rate == s->load_rate_ops_per_ms;
}
static void statistics(double *values, size_t count, P5ThreadActivityStatistics *result) {
    double sum = 0, squared = 0;
    qsort(values, count, sizeof(*values), compare_double);
    for (size_t index = 0; index < count; index++) sum += values[index];
    result->count = count; result->min = values[0]; result->max = values[count - 1]; result->mean = sum / count;
    for (size_t index = 0; index < count; index++) { double delta = values[index] - result->mean; squared += delta * delta; }
    result->median = percentile(values, count, .5); result->stddev = sqrt(squared / count); result->p10 = percentile(values, count, .1); result->p90 = percentile(values, count, .9);
}

const char *p5_thread_activity_profile_name(P5ThreadActivityProfile value) { return value == P5_THREAD_ACTIVITY_PROFILE_LOW ? "LOW" : value == P5_THREAD_ACTIVITY_PROFILE_MID ? "MID" : value == P5_THREAD_ACTIVITY_PROFILE_HIGH ? "HIGH" : "UNKNOWN"; }
const char *p5_thread_activity_class_name(P5ThreadActivityClass value) { return value == P5_THREAD_ACTIVITY_COLD ? "COLD" : value == P5_THREAD_ACTIVITY_MODERATE ? "MODERATE" : value == P5_THREAD_ACTIVITY_HOT ? "HOT" : "UNAVAILABLE"; }
const char *p5_thread_activity_calibration_status_name(P5ThreadActivityCalibrationStatus value) { return value == P5_THREAD_ACTIVITY_CALIBRATION_VALID ? "VALID" : value == P5_THREAD_ACTIVITY_CALIBRATION_NOT_SEPARABLE ? "NOT_SEPARABLE" : value == P5_THREAD_ACTIVITY_CALIBRATION_INSUFFICIENT_SAMPLES ? "INSUFFICIENT_SAMPLES" : value == P5_THREAD_ACTIVITY_CALIBRATION_INVALID_CONTEXT ? "INVALID_CONTEXT" : "MALFORMED"; }
bool p5_thread_activity_hardware_fingerprint(const P5ThreadActivityContext *c, char *out, size_t size) { int written; if (!c || !out || !size || !text(c->architecture) || !text(c->cpu_vendor) || !c->online_cpu_count || !c->numa_node_count || !c->page_size) return false; written = snprintf(out, size, "arch=%s|vendor=%s|family=%u|model=%u|cpus=%u|numa=%u|page=%llu", c->architecture, c->cpu_vendor, c->cpu_family, c->cpu_model, c->online_cpu_count, c->numa_node_count, (unsigned long long)c->page_size); return written >= 0 && (size_t)written < size; }
bool p5_thread_activity_rate(uint64_t delta, uint64_t interval, double *out) { double rate; if (!out || !interval) return false; rate = (double)delta / interval; if (!isfinite(rate)) return false; *out = rate; return true; }

bool p5_thread_activity_calibration_context_matches(const P5ThreadActivityContext *a, const P5ThreadActivityContext *b, char *out, size_t size) {
    if (!context_shape(a) || !context_shape(b)) { set_reason(out, size, "invalid_context"); return false; }
#define MATCH_FIELD(field, label) if (a->field != b->field) { set_reason(out, size, label); return false; }
#define MATCH_TEXT(field, label) if (!same(a->field, b->field)) { set_reason(out, size, label); return false; }
    MATCH_FIELD(schema_version, "schema_version_mismatch"); MATCH_TEXT(hardware_fingerprint, "hardware_fingerprint_mismatch"); MATCH_TEXT(architecture, "architecture_mismatch"); MATCH_FIELD(numa_node_count, "numa_node_count_mismatch"); MATCH_FIELD(page_size, "page_size_mismatch"); MATCH_TEXT(metric_name, "metric_name_mismatch"); MATCH_FIELD(metric_version, "metric_version_mismatch"); MATCH_TEXT(metric_unit, "metric_unit_mismatch"); MATCH_FIELD(worker_count, "worker_count_mismatch"); MATCH_FIELD(memory_mb, "memory_mb_mismatch"); MATCH_FIELD(placement_mode, "placement_mode_mismatch"); MATCH_TEXT(benchmark_definition_version, "benchmark_definition_version_mismatch"); MATCH_TEXT(intensity_profile_version, "intensity_profile_version_mismatch"); MATCH_FIELD(low_intensity_percent, "low_intensity_percent_mismatch"); MATCH_FIELD(mid_intensity_percent, "mid_intensity_percent_mismatch"); MATCH_FIELD(high_intensity_percent, "high_intensity_percent_mismatch"); MATCH_FIELD(numa_balancing_state, "numa_balancing_state_mismatch"); MATCH_FIELD(window_interval_count, "window_interval_count_mismatch"); MATCH_FIELD(minimum_worker_run_units, "minimum_worker_run_units_mismatch"); MATCH_FIELD(benchmark_seed, "benchmark_seed_mismatch");
#undef MATCH_FIELD
#undef MATCH_TEXT
    set_reason(out, size, "match"); return true;
}

int p5_thread_activity_calibration_derive_worker_runs(const P5ThreadActivityContext *context, const char *id, const P5ThreadActivityRawSample *samples, size_t sample_count, P5ThreadActivityWorkerRun **out_units, size_t *out_count, char *out_reason, size_t reason_size) {
    P5ThreadActivityWorkerRun *units = NULL; size_t count = 0, capacity = 0;
    if (!out_units || !out_count || !context_shape(context) || !text(id) || !samples || !sample_count) { set_reason(out_reason, reason_size, "invalid_context"); return -1; }
    *out_units = NULL; *out_count = 0;
    for (size_t i = 0; i < sample_count; i++) { if (!valid_row(context, id, &samples[i])) goto malformed; for (size_t j = 0; j < i; j++) if (samples[j].run_index == samples[i].run_index && samples[j].worker_index == samples[i].worker_index && samples[j].sample_index == samples[i].sample_index) goto malformed; }
    for (size_t base = 0; base < sample_count; base++) {
        const P5ThreadActivityRawSample *seed = &samples[base]; size_t group_count = 0, copied = 0, valid_intervals = 0, window_count = 0, interval_count = 0; bool seen = false, warm = false; uint64_t next = 0; double interval_rates[P5_THREAD_ACTIVITY_WINDOW_INTERVALS];
        for (size_t i = 0; i < base; i++) if (same_group(seed, &samples[i])) seen = true;
        if (seen) continue;
        for (size_t i = 0; i < sample_count; i++) if (same_group(seed, &samples[i])) group_count++;
        P5ThreadActivityRawSample *group = calloc(group_count, sizeof(*group)); double *medians = calloc(group_count / P5_THREAD_ACTIVITY_WINDOW_INTERVALS, sizeof(*medians));
        if (!group || (group_count >= P5_THREAD_ACTIVITY_WINDOW_INTERVALS && !medians)) { free(group); free(medians); goto malformed; }
        for (size_t i = 0; i < sample_count; i++) if (same_group(seed, &samples[i])) group[copied++] = samples[i];
        qsort(group, group_count, sizeof(*group), compare_sample);
        for (size_t i = 0; i < group_count; i++) {
            if (group[i].sample_kind == P5_THREAD_ACTIVITY_SAMPLE_WARMUP_RUN) { warm = true; break; }
            if (group[i].sample_kind != P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL || !group[i].valid) { interval_count = 0; continue; }
            valid_intervals++;
            if (interval_count && group[i].sample_index != next) interval_count = 0;
            interval_rates[interval_count++] = group[i].load_rate_ops_per_ms; next = group[i].sample_index + 1;
            if (interval_count == P5_THREAD_ACTIVITY_WINDOW_INTERVALS) { qsort(interval_rates, interval_count, sizeof(*interval_rates), compare_double); medians[window_count++] = percentile(interval_rates, interval_count, .5); interval_count = 0; }
        }
        if (!warm && window_count) {
            if (count == capacity) { size_t next_capacity = capacity ? capacity * 2 : 16; P5ThreadActivityWorkerRun *next_units = realloc(units, next_capacity * sizeof(*units)); if (!next_units) { free(group); free(medians); goto malformed; } units = next_units; capacity = next_capacity; }
            qsort(medians, window_count, sizeof(*medians), compare_double);
            units[count] = (P5ThreadActivityWorkerRun){.run_index = seed->run_index, .worker_index = seed->worker_index, .controlled_profile = seed->controlled_profile, .intensity_percent = intensity(context, seed->controlled_profile), .placement_mode = context->placement_mode, .valid_interval_count = valid_intervals, .valid_window_count = window_count, .worker_run_median_rate_ops_per_ms = percentile(medians, window_count, .5), .valid = true};
            snprintf(units[count++].reason, sizeof(units[count - 1].reason), "valid");
        }
        free(group); free(medians);
    }
    *out_units = units; *out_count = count; set_reason(out_reason, reason_size, "derived"); return 0;
malformed:
    free(units); set_reason(out_reason, reason_size, "malformed_sample"); return -1;
}

static bool build_report(const P5ThreadActivityWorkerRun *units, size_t unit_count, P5ThreadActivityProfileReport *result) {
    double *values = calloc(unit_count, sizeof(*values));
    if (!values) return false;
    for (size_t i = 0; i < unit_count; i++) values[i] = units[i].worker_run_median_rate_ops_per_ms;
    statistics(values, unit_count, &result->statistics); free(values);
    for (size_t i = 0; i < unit_count; i++) for (size_t j = i + 1; j < unit_count; j++) if (units[i].run_index == units[j].run_index && units[i].worker_index == 0 && units[j].worker_index == 1) { double mean = (fabs(units[i].worker_run_median_rate_ops_per_ms) + fabs(units[j].worker_run_median_rate_ops_per_ms)) / 2.0; if (mean > 0) { double difference = fabs(units[i].worker_run_median_rate_ops_per_ms - units[j].worker_run_median_rate_ops_per_ms) / mean; result->worker_agreement_count++; result->worker_agreement_mean += difference; if (difference > result->worker_agreement_max) result->worker_agreement_max = difference; } }
    if (result->worker_agreement_count) result->worker_agreement_mean /= result->worker_agreement_count;
    for (size_t i = 0; i < unit_count; i++) { bool first = true; size_t kept = 0, omitted = 0, index = 0; for (size_t j = 0; j < i; j++) if (units[j].run_index == units[i].run_index) first = false; if (!first) continue; for (size_t j = 0; j < unit_count; j++) if (units[j].run_index == units[i].run_index) omitted++; else kept++; if (!kept) continue; double *remaining = calloc(kept, sizeof(*remaining)); if (!remaining) return false; for (size_t j = 0; j < unit_count; j++) if (units[j].run_index != units[i].run_index) remaining[index++] = units[j].worker_run_median_rate_ops_per_ms; qsort(remaining, kept, sizeof(*remaining), compare_double); double low = percentile(remaining, kept, .1), high = percentile(remaining, kept, .9); free(remaining); for (size_t j = 0; j < unit_count; j++) if (units[j].run_index == units[i].run_index) { result->leave_one_run_out_total++; if (units[j].worker_run_median_rate_ops_per_ms >= low && units[j].worker_run_median_rate_ops_per_ms <= high) result->leave_one_run_out_in_band++; } (void)omitted; }
    if (result->leave_one_run_out_total) result->leave_one_run_out_fraction = (double)result->leave_one_run_out_in_band / result->leave_one_run_out_total;
    return true;
}

int p5_thread_activity_calibration_build(const P5ThreadActivityContext *context, const char *id, const P5ThreadActivityRawSample *samples, size_t sample_count, P5ThreadActivityCalibration *calibration, char *out_reason, size_t reason_size) {
    P5ThreadActivityWorkerRun *units = NULL; size_t unit_count = 0;
    if (!calibration) { set_reason(out_reason, reason_size, "invalid_context"); return -1; }
    memset(calibration, 0, sizeof(*calibration));
    if (p5_thread_activity_calibration_derive_worker_runs(context, id, samples, sample_count, &units, &unit_count, out_reason, reason_size) != 0) { calibration->status = context_shape(context) ? P5_THREAD_ACTIVITY_CALIBRATION_MALFORMED : P5_THREAD_ACTIVITY_CALIBRATION_INVALID_CONTEXT; return -1; }
    calibration->context = *context; snprintf(calibration->calibration_id, sizeof(calibration->calibration_id), "%s", id);
    for (int value = 0; value < 3; value++) { P5ThreadActivityProfile current = (P5ThreadActivityProfile)value; size_t count = 0, windows = 0, index = 0; for (size_t i = 0; i < unit_count; i++) if (units[i].controlled_profile == current) { count++; windows += units[i].valid_window_count; } P5ThreadActivityProfileReport *current_report = report(calibration, current); current_report->worker_run_count = count; current_report->window_count = windows; if (count < context->minimum_worker_run_units) { calibration->status = P5_THREAD_ACTIVITY_CALIBRATION_INSUFFICIENT_SAMPLES; free(units); set_reason(out_reason, reason_size, "worker_run_units_required"); return 0; } P5ThreadActivityWorkerRun *selected = calloc(count, sizeof(*selected)); if (!selected) { free(units); calibration->status = P5_THREAD_ACTIVITY_CALIBRATION_MALFORMED; set_reason(out_reason, reason_size, "allocation_failed"); return -1; } for (size_t i = 0; i < unit_count; i++) if (units[i].controlled_profile == current) selected[index++] = units[i]; if (!build_report(selected, count, current_report)) { free(selected); free(units); calibration->status = P5_THREAD_ACTIVITY_CALIBRATION_MALFORMED; set_reason(out_reason, reason_size, "allocation_failed"); return -1; } free(selected); }
    calibration->low.gap_ratio_available = calibration->low.statistics.p90 > 0; calibration->mid.gap_ratio_available = calibration->mid.statistics.p90 > 0;
    if (calibration->low.gap_ratio_available) calibration->low.gap_ratio = (calibration->mid.statistics.p10 - calibration->low.statistics.p90) / calibration->low.statistics.p90;
    if (calibration->mid.gap_ratio_available) calibration->mid.gap_ratio = (calibration->high.statistics.p10 - calibration->mid.statistics.p90) / calibration->mid.statistics.p90;
    calibration->status = calibration->low.statistics.p90 < calibration->mid.statistics.p10 && calibration->mid.statistics.p90 < calibration->high.statistics.p10 ? P5_THREAD_ACTIVITY_CALIBRATION_VALID : P5_THREAD_ACTIVITY_CALIBRATION_NOT_SEPARABLE;
    free(units); set_reason(out_reason, reason_size, p5_thread_activity_calibration_status_name(calibration->status)); return 0;
}

P5ThreadActivityClass p5_thread_activity_classify_window_rate(const P5ThreadActivityCalibration *calibration, double rate) { if (!calibration || calibration->status != P5_THREAD_ACTIVITY_CALIBRATION_VALID || !isfinite(rate)) return P5_THREAD_ACTIVITY_UNAVAILABLE; if (rate >= calibration->low.statistics.p10 && rate <= calibration->low.statistics.p90) return P5_THREAD_ACTIVITY_COLD; if (rate >= calibration->mid.statistics.p10 && rate <= calibration->mid.statistics.p90) return P5_THREAD_ACTIVITY_MODERATE; if (rate >= calibration->high.statistics.p10 && rate <= calibration->high.statistics.p90) return P5_THREAD_ACTIVITY_HOT; return P5_THREAD_ACTIVITY_UNAVAILABLE; }
