#ifndef AWAVMA_P5_THREAD_ACTIVITY_CALIBRATION_H
#define AWAVMA_P5_THREAD_ACTIVITY_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define P5_THREAD_ACTIVITY_SCHEMA_VERSION 1U
#define P5_THREAD_ACTIVITY_TEXT_MAX 128U
#define P5_THREAD_ACTIVITY_REASON_MAX 160U
#define P5_THREAD_ACTIVITY_CALIBRATION_ID_MAX 128U
#define P5_THREAD_ACTIVITY_WINDOW_INTERVALS 5U
#define P5_THREAD_ACTIVITY_MIN_WORKER_RUN_UNITS 12U
#define P5_THREAD_ACTIVITY_METRIC_NAME "registered_memory_load_rate"
#define P5_THREAD_ACTIVITY_METRIC_VERSION 1U
#define P5_THREAD_ACTIVITY_METRIC_UNIT "ops_per_ms"

typedef enum { P5_THREAD_ACTIVITY_PLACEMENT_LOCAL, P5_THREAD_ACTIVITY_PLACEMENT_REMOTE } P5ThreadActivityPlacementMode;
typedef enum { P5_THREAD_ACTIVITY_PROFILE_LOW, P5_THREAD_ACTIVITY_PROFILE_MID, P5_THREAD_ACTIVITY_PROFILE_HIGH } P5ThreadActivityProfile;
typedef enum { P5_THREAD_ACTIVITY_SAMPLE_WARMUP_RUN, P5_THREAD_ACTIVITY_SAMPLE_WARMUP_INTERVAL, P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL } P5ThreadActivitySampleKind;
typedef enum { P5_THREAD_ACTIVITY_COLD, P5_THREAD_ACTIVITY_MODERATE, P5_THREAD_ACTIVITY_HOT, P5_THREAD_ACTIVITY_UNAVAILABLE } P5ThreadActivityClass;
typedef enum { P5_THREAD_ACTIVITY_CALIBRATION_VALID, P5_THREAD_ACTIVITY_CALIBRATION_NOT_SEPARABLE, P5_THREAD_ACTIVITY_CALIBRATION_INSUFFICIENT_SAMPLES, P5_THREAD_ACTIVITY_CALIBRATION_INVALID_CONTEXT, P5_THREAD_ACTIVITY_CALIBRATION_MALFORMED } P5ThreadActivityCalibrationStatus;

typedef struct {
    uint32_t schema_version;
    char architecture[P5_THREAD_ACTIVITY_TEXT_MAX], cpu_vendor[P5_THREAD_ACTIVITY_TEXT_MAX], cpu_model_name[P5_THREAD_ACTIVITY_TEXT_MAX];
    uint32_t cpu_family, cpu_model, online_cpu_count, numa_node_count;
    uint64_t page_size;
    char hardware_fingerprint[P5_THREAD_ACTIVITY_TEXT_MAX];
    char metric_name[P5_THREAD_ACTIVITY_TEXT_MAX];
    uint32_t metric_version;
    char metric_unit[P5_THREAD_ACTIVITY_TEXT_MAX];
    uint32_t worker_count;
    uint64_t memory_mb;
    P5ThreadActivityPlacementMode placement_mode;
    uint64_t benchmark_seed;
    char benchmark_definition_version[P5_THREAD_ACTIVITY_TEXT_MAX];
    char intensity_profile_version[P5_THREAD_ACTIVITY_TEXT_MAX];
    uint32_t low_intensity_percent, mid_intensity_percent, high_intensity_percent;
    uint32_t numa_balancing_state;
    uint32_t window_interval_count, minimum_worker_run_units;
} P5ThreadActivityContext;

/* Reusable offline interval evidence deliberately excludes all live process/thread identity. */
typedef struct {
    uint32_t schema_version;
    char calibration_id[P5_THREAD_ACTIVITY_CALIBRATION_ID_MAX];
    uint64_t run_index, sample_index;
    uint32_t worker_index;
    P5ThreadActivityProfile controlled_profile;
    uint32_t intensity_percent;
    P5ThreadActivitySampleKind sample_kind;
    P5ThreadActivityPlacementMode placement_mode;
    uint32_t worker_count;
    uint64_t memory_mb, load_operations_delta, interval_ms;
    double load_rate_ops_per_ms;
    char metric_name[P5_THREAD_ACTIVITY_TEXT_MAX], metric_unit[P5_THREAD_ACTIVITY_TEXT_MAX];
    uint32_t metric_version;
    bool valid;
    char reason[P5_THREAD_ACTIVITY_REASON_MAX];
} P5ThreadActivityRawSample;

typedef struct { size_t count; double min, max, mean, median, stddev, p10, p90; } P5ThreadActivityStatistics;
typedef struct {
    size_t worker_run_count, window_count;
    P5ThreadActivityStatistics statistics;
    bool gap_ratio_available;
    double gap_ratio;
    size_t worker_agreement_count;
    double worker_agreement_mean, worker_agreement_max;
    size_t leave_one_run_out_total, leave_one_run_out_in_band;
    double leave_one_run_out_fraction;
} P5ThreadActivityProfileReport;
typedef struct {
    P5ThreadActivityContext context;
    char calibration_id[P5_THREAD_ACTIVITY_CALIBRATION_ID_MAX];
    P5ThreadActivityCalibrationStatus status;
    P5ThreadActivityProfileReport low, mid, high;
} P5ThreadActivityCalibration;

const char *p5_thread_activity_profile_name(P5ThreadActivityProfile profile);
const char *p5_thread_activity_class_name(P5ThreadActivityClass classification);
const char *p5_thread_activity_calibration_status_name(P5ThreadActivityCalibrationStatus status);
bool p5_thread_activity_hardware_fingerprint(const P5ThreadActivityContext *context, char *fingerprint, size_t fingerprint_size);
bool p5_thread_activity_rate(uint64_t load_operations_delta, uint64_t interval_ms, double *rate_ops_per_ms);
bool p5_thread_activity_calibration_context_matches(const P5ThreadActivityContext *calibration_context, const P5ThreadActivityContext *requested_context, char *reason, size_t reason_size);
/* A structurally valid but insufficient evidence set returns 0 with INSUFFICIENT_SAMPLES. */
int p5_thread_activity_calibration_build(const P5ThreadActivityContext *context, const char *calibration_id, const P5ThreadActivityRawSample *samples, size_t sample_count, P5ThreadActivityCalibration *calibration, char *reason, size_t reason_size);
/* Input is a median of exactly five consecutive valid C1 intervals, never a raw interval. */
P5ThreadActivityClass p5_thread_activity_classify_window_rate(const P5ThreadActivityCalibration *calibration, double window_median_rate);

#endif
