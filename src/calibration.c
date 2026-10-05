#include "calibration.h"

#include "page_candidate_provider.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_COUNT 46U
#define TOLERANCE 0.000001

static const char *const header[FIELD_COUNT] = {
    "schema_version","calibration_version","calibration_id","created_at_utc","collection_experiment_id","calibration_status",
    "cpu_architecture","cpu_model","online_numa_nodes","topology_fingerprint","local_node","remote_node","numa_distance","local_permitted_cpu_count","page_size_bytes",
    "benchmark_pattern","threads","memory_bytes","memory_pages","duration_seconds","action_kind","source_node","destination_node","migration_page_bucket",
    "valid_pair_count","local_mean_ms","local_stddev_ms","remote_mean_ms","remote_stddev_ms","paired_penalty_mean_ms","paired_penalty_lower_bound_ms","expected_recoverable_gain_pct","uncertainty_pct","safety_margin_pct",
    "cost_sample_count","migration_cost_mean_ms","migration_cost_stddev_ms","migration_cost_conservative_ms","estimated_cost_pct","successful_pages","failed_pages",
    "placement_evidence_schema_version","local_placement_artifact_hash","remote_placement_artifact_hash","migration_measurement_method","rejection_reason"
};

static bool record_valid(const CalibrationRecord *record, const CalibrationPolicy *policy);

static uint64_t hash_text(uint64_t hash, const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0; i < sizeof(length); i++) { hash ^= (unsigned char)(length >> (i * 8)); hash *= 1099511628211ULL; }
    for (size_t i = 0; i < length; i++) { hash ^= (unsigned char)text[i]; hash *= 1099511628211ULL; }
    return hash;
}
static bool copy_field(char *out, size_t size, const char *text)
{
    return text != NULL && *text != '\0' && strchr(text, '\r') == NULL && strchr(text, '\n') == NULL &&
           snprintf(out, size, "%s", text) < (int)size;
}
static bool number(const char *text, unsigned long long *out)
{
    char *end = NULL; unsigned long long value;
    if (text == NULL || *text == '\0' || *text == '-' || isspace((unsigned char)*text)) return false;
    errno = 0; value = strtoull(text, &end, 10);
    if (errno || end == text || *end) return false;
    *out = value;
    return true;
}
static bool integer(const char *text, int *out)
{
    char *end = NULL; long value;
    if (text == NULL || *text == '\0' || isspace((unsigned char)*text)) return false;
    errno = 0; value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < 0 || value > 1000000) return false;
    *out = (int)value;
    return true;
}
static bool decimal(const char *text, double *out)
{
    char *end = NULL; double value;
    if (text == NULL || *text == '\0' || isspace((unsigned char)*text)) return false;
    errno = 0; value = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(value)) return false;
    *out = value;
    return true;
}
static bool id_valid(const char *text, const char *prefix)
{
    if (strncmp(text, prefix, strlen(prefix)) != 0 || strlen(text) != strlen(prefix) + 16) return false;
    for (const char *p = text + strlen(prefix); *p; p++) if (!isxdigit((unsigned char)*p) || isupper((unsigned char)*p)) return false;
    return true;
}
static bool action_parse(const char *text, ValidationAction *action)
{
    if (strcmp(text, "MOVE_MEMORY") == 0) { *action = VALIDATION_ACTION_MOVE_MEMORY; return true; }
    if (strcmp(text, "MOVE_THREAD") == 0) { *action = VALIDATION_ACTION_MOVE_THREAD; return true; }
    return false;
}
static bool status_parse(const char *text, calibration_status_t *status)
{
    if (strcmp(text, "VALIDATED_PRODUCTION") == 0) { *status = CALIBRATION_VALIDATED_PRODUCTION; return true; }
    if (strcmp(text, "VALIDATED_TEST_ONLY") == 0) { *status = CALIBRATION_VALIDATED_TEST_ONLY; return true; }
    return false;
}
void calibration_policy_default(CalibrationPolicy *policy) { if (policy != NULL) *policy = (CalibrationPolicy){7, 7}; }
const char *calibration_match_status_name(calibration_match_status_t status)
{
    static const char *names[] = {"CALIBRATION_MATCHED","CALIBRATION_UNAVAILABLE","CALIBRATION_MALFORMED","CALIBRATION_SCHEMA_UNSUPPORTED","CALIBRATION_MISMATCH","CALIBRATION_ACTION_MISMATCH","CALIBRATION_WORKLOAD_MISMATCH","CALIBRATION_TOPOLOGY_MISMATCH","CALIBRATION_PAGE_SIZE_MISMATCH","CALIBRATION_BUCKET_MISMATCH","CALIBRATION_INSUFFICIENT_SAMPLES","CALIBRATION_WEAK_EVIDENCE","CALIBRATION_ID_MISMATCH"};
    return status <= CALIBRATION_ID_MISMATCH ? names[status] : "CALIBRATION_INVALID";
}
bool calibration_topology_fingerprint(const calibration_compatibility_t *key, char output[CALIBRATION_ID_MAX])
{
    char values[512]; uint64_t hash = 1469598103934665603ULL;
    if (key == NULL || output == NULL || key->online_numa_nodes == 0 || key->local_node < 0 || key->remote_node < 0 || key->numa_distance <= 0 || key->page_size_bytes == 0) return false;
    snprintf(values, sizeof(values), "%s|%s|%u|%d|%d|%d|%u|%zu", key->cpu_architecture, key->cpu_model, key->online_numa_nodes, key->local_node, key->remote_node, key->numa_distance, key->local_permitted_cpu_count, key->page_size_bytes);
    hash = hash_text(hash, "AWAVMA:topology:v1"); hash = hash_text(hash, values);
    return snprintf(output, CALIBRATION_ID_MAX, "top1-%016llx", (unsigned long long)hash) < (int)CALIBRATION_ID_MAX;
}
bool calibration_record_id(const CalibrationRecord *record, char output[CALIBRATION_ID_MAX])
{
    char data[2048], topology[CALIBRATION_ID_MAX]; uint64_t hash = 1469598103934665603ULL;
    if (record == NULL || output == NULL || !calibration_topology_fingerprint(&record->compatibility, topology)) return false;
    snprintf(data, sizeof(data), "%u|%s|%s|%s|%s|%s|%s|%u|%zu|%zu|%.9g|%d|%d|%zu|%zu|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|%zu|%.9g|%.9g|%.9g|%.9g|%zu|%zu|%s|%s|%s", record->schema_version, record->calibration_version, record->created_at_utc, record->collection_experiment_id, topology, record->workload.benchmark_pattern, record->action == VALIDATION_ACTION_MOVE_MEMORY ? "MOVE_MEMORY" : "MOVE_THREAD", record->workload.threads, record->workload.memory_bytes, record->workload.memory_pages, record->workload.duration_seconds, record->source_node, record->destination_node, record->migration_page_bucket, record->valid_pair_count, record->local_mean_ms, record->local_stddev_ms, record->remote_mean_ms, record->remote_stddev_ms, record->paired_penalty_mean_ms, record->paired_penalty_lower_bound_ms, record->expected_recoverable_gain_pct, record->uncertainty_pct, record->safety_margin_pct, record->cost_sample_count, record->migration_cost_mean_ms, record->migration_cost_stddev_ms, record->migration_cost_conservative_ms, record->estimated_cost_pct, record->successful_pages, record->failed_pages, record->local_placement_artifact_hash, record->remote_placement_artifact_hash, record->migration_measurement_method);
    hash = hash_text(hash, "AWAVMA:calibration:record:v1"); hash = hash_text(hash, data);
    return snprintf(output, CALIBRATION_ID_MAX, "cal1-%016llx", (unsigned long long)hash) < (int)CALIBRATION_ID_MAX;
}
int calibration_write_csv(const char *path, const CalibrationRecord *r)
{
    FILE *file;
    if (path == NULL || r == NULL || !record_valid(r, &(CalibrationPolicy){1, 1})) return -1;
    file = fopen(path, "w");
    if (file == NULL) return -1;
    for (size_t i = 0; i < FIELD_COUNT; i++) fprintf(file, "%s%s", i == 0 ? "" : ",", header[i]);
    fputc('\n', file);
    fprintf(file, "%u,%s,%s,%s,%s,%s,%s,%s,%u,%s,%d,%d,%d,%u,%zu,%s,%u,%zu,%zu,%.9g,%s,%d,%d,%zu,%zu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%zu,%.9g,%.9g,%.9g,%.9g,%zu,%zu,%u,%s,%s,%s,%s\n",
        r->schema_version, r->calibration_version, r->calibration_id, r->created_at_utc, r->collection_experiment_id,
        r->status == CALIBRATION_VALIDATED_PRODUCTION ? "VALIDATED_PRODUCTION" : "VALIDATED_TEST_ONLY", r->compatibility.cpu_architecture,
        r->compatibility.cpu_model, r->compatibility.online_numa_nodes, r->compatibility.topology_fingerprint,
        r->compatibility.local_node, r->compatibility.remote_node, r->compatibility.numa_distance,
        r->compatibility.local_permitted_cpu_count, r->compatibility.page_size_bytes, r->workload.benchmark_pattern,
        r->workload.threads, r->workload.memory_bytes, r->workload.memory_pages, r->workload.duration_seconds,
        r->action == VALIDATION_ACTION_MOVE_MEMORY ? "MOVE_MEMORY" : "MOVE_THREAD", r->source_node, r->destination_node,
        r->migration_page_bucket, r->valid_pair_count, r->local_mean_ms, r->local_stddev_ms, r->remote_mean_ms,
        r->remote_stddev_ms, r->paired_penalty_mean_ms, r->paired_penalty_lower_bound_ms, r->expected_recoverable_gain_pct,
        r->uncertainty_pct, r->safety_margin_pct, r->cost_sample_count, r->migration_cost_mean_ms,
        r->migration_cost_stddev_ms, r->migration_cost_conservative_ms, r->estimated_cost_pct, r->successful_pages,
        r->failed_pages, r->placement_evidence_schema_version, r->local_placement_artifact_hash,
        r->remote_placement_artifact_hash, r->migration_measurement_method, r->rejection_reason);
    return fclose(file) == 0 ? 0 : -1;
}
static bool record_valid(const CalibrationRecord *r, const CalibrationPolicy *p)
{
    char topology[CALIBRATION_ID_MAX], id[CALIBRATION_ID_MAX]; double gain, cost;
    if (r->schema_version != CALIBRATION_SCHEMA_VERSION || !id_valid(r->calibration_id, "cal1-") ||
        !id_valid(r->compatibility.topology_fingerprint, "top1-") || !id_valid(r->local_placement_artifact_hash, "art1-") ||
        !id_valid(r->remote_placement_artifact_hash, "art1-") || !calibration_topology_fingerprint(&r->compatibility, topology) ||
        strcmp(topology, r->compatibility.topology_fingerprint) != 0 || !calibration_record_id(r, id) || strcmp(id, r->calibration_id) != 0 ||
        r->compatibility.online_numa_nodes < 2 || r->compatibility.local_node < 0 || r->compatibility.remote_node < 0 ||
        r->compatibility.local_node == r->compatibility.remote_node || r->compatibility.numa_distance <= 0 ||
        r->compatibility.local_permitted_cpu_count == 0 || r->workload.threads == 0 || r->workload.memory_bytes == 0 || r->workload.memory_pages == 0 ||
        r->compatibility.page_size_bytes < 1024 || (r->compatibility.page_size_bytes & (r->compatibility.page_size_bytes - 1)) != 0 ||
        r->workload.memory_bytes / r->compatibility.page_size_bytes != r->workload.memory_pages || r->workload.memory_bytes % r->compatibility.page_size_bytes != 0 ||
        r->workload.duration_seconds <= 0 || r->source_node < 0 || r->destination_node < 0 || r->source_node == r->destination_node ||
        r->valid_pair_count < p->minimum_pairs || r->cost_sample_count < p->minimum_cost_samples || r->remote_mean_ms <= 0 || r->local_mean_ms < 0 ||
        r->paired_penalty_lower_bound_ms <= 0 || r->expected_recoverable_gain_pct <= 0 || r->expected_recoverable_gain_pct > 100 ||
        r->estimated_cost_pct < 0 || r->estimated_cost_pct > 100 || r->uncertainty_pct < 0 || r->uncertainty_pct > 100 || r->safety_margin_pct < 0 || r->safety_margin_pct > 100 ||
        r->estimated_cost_pct + r->uncertainty_pct + r->safety_margin_pct > 100 ||
        r->migration_cost_conservative_ms < 0 || r->local_stddev_ms < 0 || r->remote_stddev_ms < 0 || r->migration_cost_mean_ms < 0 || r->migration_cost_stddev_ms < 0)
        return false;
    if (r->action == VALIDATION_ACTION_MOVE_MEMORY) {
        if (r->migration_page_bucket == 0 || r->migration_page_bucket > PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST ||
            r->successful_pages + r->failed_pages != r->migration_page_bucket ||
            strcmp(r->migration_measurement_method, "move_pages") != 0) return false;
    } else if (r->action == VALIDATION_ACTION_MOVE_THREAD) {
        if (r->migration_page_bucket != 0 || r->successful_pages != 0 || r->failed_pages != 0 ||
            strcmp(r->migration_measurement_method, "sched_setaffinity") != 0) return false;
    } else return false;
    gain = r->paired_penalty_lower_bound_ms / r->remote_mean_ms * 100.0;
    cost = r->migration_cost_conservative_ms / r->remote_mean_ms * 100.0;
    return fabs(gain - r->expected_recoverable_gain_pct) <= TOLERANCE && fabs(cost - r->estimated_cost_pct) <= TOLERANCE;
}
static int split(char *line, char *fields[FIELD_COUNT])
{
    size_t count = 0; char *cursor = line;
    if (strchr(line, '"') != NULL) return -1;
    while (cursor != NULL && count < FIELD_COUNT) { fields[count++] = cursor; cursor = strchr(cursor, ','); if (cursor != NULL) *cursor++ = '\0'; }
    return cursor == NULL && count == FIELD_COUNT ? (int)count : -1;
}
static bool parse_record(char *fields[FIELD_COUNT], CalibrationRecord *r)
{
    unsigned long long value;
#define U(I, FIELD) do { if (!number(fields[I], &value) || value > SIZE_MAX) return false; r->FIELD = (size_t)value; } while (0)
#define D(I, FIELD) do { if (!decimal(fields[I], &r->FIELD)) return false; } while (0)
    memset(r, 0, sizeof(*r));
    if (!number(fields[0], &value) || value != CALIBRATION_SCHEMA_VERSION || !copy_field(r->calibration_version, sizeof(r->calibration_version), fields[1]) || !copy_field(r->calibration_id, sizeof(r->calibration_id), fields[2]) || !copy_field(r->created_at_utc, sizeof(r->created_at_utc), fields[3]) || !copy_field(r->collection_experiment_id, sizeof(r->collection_experiment_id), fields[4]) || !status_parse(fields[5], &r->status) || !copy_field(r->compatibility.cpu_architecture, sizeof(r->compatibility.cpu_architecture), fields[6]) || !copy_field(r->compatibility.cpu_model, sizeof(r->compatibility.cpu_model), fields[7]) || !number(fields[8], &value) || value > UINT_MAX || !copy_field(r->compatibility.topology_fingerprint, sizeof(r->compatibility.topology_fingerprint), fields[9])) return false;
    r->schema_version = (unsigned)value; /* overwritten below after node parsing only to keep conversion explicit */
    r->schema_version = CALIBRATION_SCHEMA_VERSION; r->compatibility.online_numa_nodes = (unsigned)value;
    if (!integer(fields[10], &r->compatibility.local_node) || !integer(fields[11], &r->compatibility.remote_node) || !integer(fields[12], &r->compatibility.numa_distance) || !number(fields[13], &value) || value > UINT_MAX || !number(fields[14], &value) || value == 0 || value > SIZE_MAX || !copy_field(r->workload.benchmark_pattern, sizeof(r->workload.benchmark_pattern), fields[15]) || !number(fields[16], &value) || value == 0 || value > UINT_MAX) return false;
    r->compatibility.local_permitted_cpu_count = (unsigned)value; r->compatibility.page_size_bytes = (size_t)value; r->workload.threads = (unsigned)value;
    /* Reparse independently to avoid accepting overflow through shared temporaries. */
    if (!number(fields[13], &value) || value > UINT_MAX) return false;
    r->compatibility.local_permitted_cpu_count = (unsigned)value;
    if (!number(fields[14], &value) || value == 0 || value > SIZE_MAX) return false;
    r->compatibility.page_size_bytes = (size_t)value;
    if (!number(fields[16], &value) || value == 0 || value > UINT_MAX) return false;
    r->workload.threads = (unsigned)value;
    U(17, workload.memory_bytes); U(18, workload.memory_pages); D(19, workload.duration_seconds);
    if (!action_parse(fields[20], &r->action) || !integer(fields[21], &r->source_node) || !integer(fields[22], &r->destination_node)) return false;
    U(23, migration_page_bucket); U(24, valid_pair_count); D(25, local_mean_ms); D(26, local_stddev_ms); D(27, remote_mean_ms); D(28, remote_stddev_ms); D(29, paired_penalty_mean_ms); D(30, paired_penalty_lower_bound_ms); D(31, expected_recoverable_gain_pct); D(32, uncertainty_pct); D(33, safety_margin_pct); U(34, cost_sample_count); D(35, migration_cost_mean_ms); D(36, migration_cost_stddev_ms); D(37, migration_cost_conservative_ms); D(38, estimated_cost_pct); U(39, successful_pages); U(40, failed_pages);
    if (!number(fields[41], &value) || value > UINT_MAX || !copy_field(r->local_placement_artifact_hash, sizeof(r->local_placement_artifact_hash), fields[42]) || !copy_field(r->remote_placement_artifact_hash, sizeof(r->remote_placement_artifact_hash), fields[43]) || !copy_field(r->migration_measurement_method, sizeof(r->migration_measurement_method), fields[44]) || !copy_field(r->rejection_reason, sizeof(r->rejection_reason), fields[45])) return false;
    r->placement_evidence_schema_version = (unsigned)value;
#undef U
#undef D
    return true;
}
int calibration_load_csv(const char *path, const CalibrationPolicy *policy, CalibrationSnapshot *snapshot,
                         calibration_match_status_t *status, char reason[CALIBRATION_REASON_MAX])
{
    CalibrationPolicy defaults, active; FILE *file; char line[8192], *fields[FIELD_COUNT]; size_t count = 0;
    if (snapshot == NULL || status == NULL || reason == NULL) return -1;
    memset(snapshot, 0, sizeof(*snapshot)); *status = CALIBRATION_UNAVAILABLE; snprintf(reason, CALIBRATION_REASON_MAX, "CALIBRATION_UNAVAILABLE");
    if (path == NULL || *path == '\0') return 0;
    calibration_policy_default(&defaults); active = policy == NULL ? defaults : *policy;
    if (active.minimum_pairs == 0 || active.minimum_cost_samples == 0 || (file = fopen(path, "r")) == NULL) { *status = CALIBRATION_MALFORMED; snprintf(reason, CALIBRATION_REASON_MAX, "CALIBRATION_OPEN_FAILED"); return -1; }
    if (fgets(line, sizeof(line), file) == NULL || strpbrk(line, "\r\n") == NULL ||
        (line[strcspn(line, "\r\n")] = '\0', split(line, fields) < 0)) {
        *status = CALIBRATION_SCHEMA_UNSUPPORTED;
        snprintf(reason, CALIBRATION_REASON_MAX, "CALIBRATION_SCHEMA_UNSUPPORTED");
        goto fail;
    }
    for (size_t i = 0; i < FIELD_COUNT; i++) if (strcmp(fields[i], header[i]) != 0) { *status = CALIBRATION_SCHEMA_UNSUPPORTED; snprintf(reason, CALIBRATION_REASON_MAX, "CALIBRATION_SCHEMA_UNSUPPORTED"); goto fail; }
    while (fgets(line, sizeof(line), file) != NULL) {
        CalibrationRecord record;
        if (strpbrk(line, "\r\n") == NULL) goto malformed;
        line[strcspn(line, "\r\n")] = '\0'; if (split(line, fields) < 0 || !parse_record(fields, &record) || !record_valid(&record, &active)) goto malformed;
        for (size_t i = 0; i < count; i++) if (strcmp(snapshot->records[i].calibration_id, record.calibration_id) == 0 || (snapshot->records[i].action == record.action && memcmp(&snapshot->records[i].compatibility, &record.compatibility, sizeof(record.compatibility)) == 0 && memcmp(&snapshot->records[i].workload, &record.workload, sizeof(record.workload)) == 0 && snapshot->records[i].source_node == record.source_node && snapshot->records[i].destination_node == record.destination_node && snapshot->records[i].migration_page_bucket == record.migration_page_bucket)) goto malformed;
        CalibrationRecord *expanded = realloc(snapshot->records, (count + 1) * sizeof(*expanded)); if (expanded == NULL) goto malformed; snapshot->records = expanded; snapshot->records[count++] = record;
    }
    fclose(file); if (count == 0) goto malformed_no_file; snapshot->count = count; *status = CALIBRATION_MATCHED; snprintf(reason, CALIBRATION_REASON_MAX, "CALIBRATION_LOADED"); return 0;
malformed: *status = CALIBRATION_MALFORMED; snprintf(reason, CALIBRATION_REASON_MAX, "CALIBRATION_MALFORMED");
fail: fclose(file); calibration_snapshot_release(snapshot); return -1;
malformed_no_file: *status = CALIBRATION_MALFORMED; snprintf(reason, CALIBRATION_REASON_MAX, "CALIBRATION_EMPTY"); calibration_snapshot_release(snapshot); return -1;
}
void calibration_snapshot_release(CalibrationSnapshot *snapshot) { if (snapshot != NULL) { free(snapshot->records); memset(snapshot, 0, sizeof(*snapshot)); } }
static bool same_workload(const calibration_workload_t *a, const calibration_workload_t *b) { return strcmp(a->benchmark_pattern, b->benchmark_pattern) == 0 && a->threads == b->threads && a->memory_bytes == b->memory_bytes && a->memory_pages == b->memory_pages && fabs(a->duration_seconds - b->duration_seconds) <= TOLERANCE; }
calibration_match_status_t calibration_match(const CalibrationSnapshot *snapshot, const CalibrationMatchRequest *request, ValidatedCalibrationMatch *result)
{
    if (result == NULL) return CALIBRATION_MALFORMED;
    memset(result, 0, sizeof(*result)); result->status = CALIBRATION_UNAVAILABLE; snprintf(result->reason, sizeof(result->reason), "CALIBRATION_UNAVAILABLE");
    if (snapshot == NULL || request == NULL || snapshot->count == 0) return result->status;
    for (size_t i = 0; i < snapshot->count; i++) {
        const CalibrationRecord *r = &snapshot->records[i];
        if (r->action != request->action) { result->status = CALIBRATION_ACTION_MISMATCH; continue; }
        if (strcmp(r->compatibility.cpu_architecture, request->compatibility.cpu_architecture) || strcmp(r->compatibility.cpu_model, request->compatibility.cpu_model) || r->compatibility.online_numa_nodes != request->compatibility.online_numa_nodes || strcmp(r->compatibility.topology_fingerprint, request->compatibility.topology_fingerprint) || r->compatibility.local_node != request->compatibility.local_node || r->compatibility.remote_node != request->compatibility.remote_node || r->compatibility.numa_distance != request->compatibility.numa_distance || r->compatibility.local_permitted_cpu_count != request->compatibility.local_permitted_cpu_count) { result->status = CALIBRATION_TOPOLOGY_MISMATCH; continue; }
        if (r->compatibility.page_size_bytes != request->compatibility.page_size_bytes) { result->status = CALIBRATION_PAGE_SIZE_MISMATCH; continue; }
        if (!same_workload(&r->workload, &request->workload)) { result->status = CALIBRATION_WORKLOAD_MISMATCH; continue; }
        if (r->source_node != request->source_node || r->destination_node != request->destination_node) { result->status = CALIBRATION_MISMATCH; continue; }
        if (r->action == VALIDATION_ACTION_MOVE_MEMORY && r->migration_page_bucket != request->migration_page_bucket) { result->status = CALIBRATION_BUCKET_MISMATCH; continue; }
        result->status = CALIBRATION_MATCHED; snprintf(result->reason, sizeof(result->reason), "CALIBRATION_MATCHED"); snprintf(result->calibration_id, sizeof(result->calibration_id), "%s", r->calibration_id); snprintf(result->calibration_version, sizeof(result->calibration_version), "%s", r->calibration_version); snprintf(result->topology_fingerprint, sizeof(result->topology_fingerprint), "%s", r->compatibility.topology_fingerprint); result->action = r->action; result->expected_gain_pct = r->expected_recoverable_gain_pct; result->base_cost_pct = r->estimated_cost_pct; result->uncertainty_pct = r->uncertainty_pct; result->safety_margin_pct = r->safety_margin_pct; result->effective_cost_pct = r->estimated_cost_pct + r->uncertainty_pct + r->safety_margin_pct; result->valid_pair_count = r->valid_pair_count; result->cost_sample_count = r->cost_sample_count; result->source_node = r->source_node; result->destination_node = r->destination_node; result->migration_page_bucket = r->migration_page_bucket; return result->status;
    }
    snprintf(result->reason, sizeof(result->reason), "%s", calibration_match_status_name(result->status)); return result->status;
}
