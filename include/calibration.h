#ifndef AWAVMA_CALIBRATION_H
#define AWAVMA_CALIBRATION_H

#include "validation_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CALIBRATION_SCHEMA_VERSION 1U
#define CALIBRATION_TEXT_MAX 128U
#define CALIBRATION_ID_MAX 32U
#define CALIBRATION_REASON_MAX 160U

typedef enum {
    CALIBRATION_VALIDATED_PRODUCTION,
    CALIBRATION_VALIDATED_TEST_ONLY
} calibration_status_t;

typedef enum {
    CALIBRATION_MATCHED,
    CALIBRATION_UNAVAILABLE,
    CALIBRATION_MALFORMED,
    CALIBRATION_SCHEMA_UNSUPPORTED,
    CALIBRATION_MISMATCH,
    CALIBRATION_ACTION_MISMATCH,
    CALIBRATION_WORKLOAD_MISMATCH,
    CALIBRATION_TOPOLOGY_MISMATCH,
    CALIBRATION_PAGE_SIZE_MISMATCH,
    CALIBRATION_BUCKET_MISMATCH,
    CALIBRATION_INSUFFICIENT_SAMPLES,
    CALIBRATION_WEAK_EVIDENCE,
    CALIBRATION_ID_MISMATCH
} calibration_match_status_t;

typedef struct {
    char cpu_architecture[CALIBRATION_TEXT_MAX];
    char cpu_model[CALIBRATION_TEXT_MAX];
    unsigned online_numa_nodes;
    char topology_fingerprint[CALIBRATION_ID_MAX];
    int local_node;
    int remote_node;
    int numa_distance;
    unsigned local_permitted_cpu_count;
    size_t page_size_bytes;
} calibration_compatibility_t;

typedef struct {
    char benchmark_pattern[CALIBRATION_TEXT_MAX];
    unsigned threads;
    size_t memory_bytes;
    size_t memory_pages;
    double duration_seconds;
} calibration_workload_t;

typedef struct {
    unsigned schema_version;
    char calibration_version[CALIBRATION_TEXT_MAX];
    char calibration_id[CALIBRATION_ID_MAX];
    char created_at_utc[CALIBRATION_TEXT_MAX];
    char collection_experiment_id[CALIBRATION_TEXT_MAX];
    calibration_status_t status;
    calibration_compatibility_t compatibility;
    calibration_workload_t workload;
    ValidationAction action;
    int source_node;
    int destination_node;
    size_t migration_page_bucket;
    size_t valid_pair_count;
    double local_mean_ms, local_stddev_ms, remote_mean_ms, remote_stddev_ms;
    double paired_penalty_mean_ms, paired_penalty_lower_bound_ms;
    double expected_recoverable_gain_pct, uncertainty_pct, safety_margin_pct;
    size_t cost_sample_count;
    double migration_cost_mean_ms, migration_cost_stddev_ms, migration_cost_conservative_ms;
    double estimated_cost_pct;
    size_t successful_pages, failed_pages;
    unsigned placement_evidence_schema_version;
    char local_placement_artifact_hash[CALIBRATION_ID_MAX];
    char remote_placement_artifact_hash[CALIBRATION_ID_MAX];
    char migration_measurement_method[CALIBRATION_TEXT_MAX];
    char rejection_reason[CALIBRATION_TEXT_MAX];
} CalibrationRecord;

typedef struct {
    CalibrationRecord *records;
    size_t count;
} CalibrationSnapshot;

typedef struct {
    calibration_compatibility_t compatibility;
    calibration_workload_t workload;
    ValidationAction action;
    int source_node;
    int destination_node;
    size_t migration_page_bucket;
} CalibrationMatchRequest;

typedef struct {
    calibration_match_status_t status;
    char reason[CALIBRATION_REASON_MAX];
    char calibration_id[CALIBRATION_ID_MAX];
    char calibration_version[CALIBRATION_TEXT_MAX];
    ValidationAction action;
    double expected_gain_pct;
    double base_cost_pct;
    double uncertainty_pct;
    double safety_margin_pct;
    double effective_cost_pct;
    size_t valid_pair_count, cost_sample_count;
    int source_node, destination_node;
    size_t migration_page_bucket;
    char topology_fingerprint[CALIBRATION_ID_MAX];
} ValidatedCalibrationMatch;

typedef struct { size_t minimum_pairs, minimum_cost_samples; } CalibrationPolicy;

void calibration_policy_default(CalibrationPolicy *policy);
const char *calibration_match_status_name(calibration_match_status_t status);
bool calibration_topology_fingerprint(const calibration_compatibility_t *compatibility,
                                      char output[CALIBRATION_ID_MAX]);
bool calibration_record_id(const CalibrationRecord *record, char output[CALIBRATION_ID_MAX]);
int calibration_write_csv(const char *path, const CalibrationRecord *record);
int calibration_load_csv(const char *path, const CalibrationPolicy *policy, CalibrationSnapshot *snapshot,
                         calibration_match_status_t *status, char reason[CALIBRATION_REASON_MAX]);
void calibration_snapshot_release(CalibrationSnapshot *snapshot);
calibration_match_status_t calibration_match(const CalibrationSnapshot *snapshot,
                                             const CalibrationMatchRequest *request,
                                             ValidatedCalibrationMatch *result);

#endif
