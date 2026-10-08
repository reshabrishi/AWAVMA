#define _POSIX_C_SOURCE 200809L
#include "p5_opportunity.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void finish(p5_opportunity_result_t *result, p5_opportunity_status_t status,
                   const char *reason)
{
    result->status = status;
    result->beneficial = status == P5_OPPORTUNITY_BENEFICIAL;
    snprintf(result->reason, sizeof(result->reason), "%s", reason);
}

const char *p5_opportunity_status_name(p5_opportunity_status_t status)
{
    static const char *names[] = {"BENEFICIAL_OPPORTUNITY", "NOT_BENEFICIAL",
        "INSUFFICIENT_EVIDENCE", "CANDIDATE_UNAVAILABLE", "PLACEMENT_UNAVAILABLE",
        "GAIN_UNAVAILABLE", "COST_UNAVAILABLE", "DECISION_UNAVAILABLE", "STALE_IDENTITY",
        "UNSUPPORTED"};
    return status <= P5_OPPORTUNITY_UNSUPPORTED ? names[status] : "UNSUPPORTED";
}

const char *p5_opportunity_memory_relation(size_t total_pages, size_t queryable_pages,
                                           size_t unknown_pages, size_t dominant_pages,
                                           int dominant_node, int candidate_node)
{
    if (total_pages == 0 || queryable_pages != total_pages || unknown_pages != 0 ||
        dominant_node < 0 || candidate_node < 0)
        return "UNKNOWN";
    if (dominant_pages * 2 <= total_pages)
        return "MIXED";
    return dominant_node == candidate_node ? "LOCAL" : "REMOTE";
}

bool p5_opportunity_select_destination(int source_cpu, int target_node,
                                       const cpu_set_t *allowed_affinity,
                                       const int cpu_nodes[CPU_SETSIZE],
                                       int *proposed_cpu, int *proposed_node)
{
    int selected_cpu = -1;

    if (source_cpu < 0 || target_node < 0 || allowed_affinity == NULL || cpu_nodes == NULL ||
        proposed_cpu == NULL || proposed_node == NULL)
        return false;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, allowed_affinity) || cpu == source_cpu || cpu_nodes[cpu] != target_node)
            continue;
        selected_cpu = cpu;
        break;
    }
    if (selected_cpu < 0)
        return false;
    *proposed_cpu = selected_cpu;
    *proposed_node = target_node;
    return true;
}

int p5_opportunity_evaluate(const p5_opportunity_input_t *input,
                            p5_opportunity_result_t *result)
{
    if (input == NULL || result == NULL || input->app_id == NULL || input->app_id[0] == '\0')
        return -1;
    memset(result, 0, sizeof(*result));
    result->pid = input->pid;
    result->start_time_ticks = input->start_time_ticks;
    result->temporal_generation = input->temporal_generation;
    result->candidate_tid = input->candidate_tid;
    result->candidate_tid_start_time_ticks = input->candidate_tid_start_time_ticks;
    result->source_cpu = input->source_cpu;
    result->source_node = input->source_node;
    result->proposed_cpu = input->proposed_cpu;
    result->proposed_node = input->proposed_node;
    result->classification_available = input->classification_available;
    result->placement_available = input->placement_available;
    result->destination_available = input->destination_available;
    result->memory_total_pages = input->memory_total_pages;
    result->memory_queryable_pages = input->memory_queryable_pages;
    result->memory_dominant_node = input->memory_dominant_node;
    result->expected_gain_available = input->expected_gain_available;
    result->migration_cost_available = input->migration_cost_available;
    result->decision_available = input->decision_available;
    result->expected_gain_ms = input->expected_gain_ms;
    result->migration_cost_ms = input->migration_cost_ms;
    snprintf(result->app_id, sizeof(result->app_id), "%s", input->app_id);
    snprintf(result->classification, sizeof(result->classification), "%s",
             input->classification_available && input->classification != NULL ? input->classification : "NA");
    snprintf(result->placement_relation, sizeof(result->placement_relation), "%s",
             input->placement_available && input->placement_relation != NULL ? input->placement_relation : "NA");
    if (input->pid <= 0 || input->start_time_ticks == 0 || input->temporal_generation == 0 ||
        !input->identity_match || !input->generation_current) {
        finish(result, P5_OPPORTUNITY_STALE_IDENTITY, "identity_or_temporal_generation_stale");
    } else if (!input->candidate_available || !input->candidate_verified || input->candidate_tid <= 0) {
        finish(result, P5_OPPORTUNITY_CANDIDATE_UNAVAILABLE, "verified_candidate_tid_unavailable");
    } else if (!input->classification_available || input->classification == NULL ||
               (strcmp(input->classification, "HOT") != 0 &&
                strcmp(input->classification, "MODERATE") != 0 &&
                strcmp(input->classification, "COLD") != 0) ||
               !input->classification_candidate_bound) {
        finish(result, P5_OPPORTUNITY_INSUFFICIENT_EVIDENCE, "candidate_bound_classification_unavailable");
    } else if (!input->placement_available || input->placement_relation == NULL || !input->source_available ||
                strcmp(input->placement_relation, "REMOTE") != 0) {
        finish(result, P5_OPPORTUNITY_PLACEMENT_UNAVAILABLE, "candidate_memory_remote_relation_unavailable");
    } else if (!input->destination_available || !input->destination_permitted || !input->affinity_valid ||
               input->proposed_cpu == input->source_cpu || input->proposed_node == input->source_node) {
        finish(result, P5_OPPORTUNITY_PLACEMENT_UNAVAILABLE, "valid_distinct_permitted_destination_unavailable");
    } else if (!input->expected_gain_available || !isfinite(input->expected_gain_ms) || input->expected_gain_ms < 0.0) {
        finish(result, P5_OPPORTUNITY_GAIN_UNAVAILABLE, "expected_gain_unavailable");
    } else if (!input->migration_cost_available || !isfinite(input->migration_cost_ms) || input->migration_cost_ms < 0.0) {
        finish(result, P5_OPPORTUNITY_COST_UNAVAILABLE, "calibrated_migration_cost_unavailable");
    } else if (!input->decision_available || !isfinite(input->memory_score) || !isfinite(input->thread_score) ||
               !isfinite(input->epsilon) || input->epsilon < 0.0) {
        finish(result, P5_OPPORTUNITY_DECISION_UNAVAILABLE, "existing_decision_evidence_unavailable");
    } else {
        result->decision_score = input->thread_score - input->memory_score;
        if (input->thread_score <= input->memory_score + input->epsilon)
            finish(result, P5_OPPORTUNITY_NOT_BENEFICIAL, "existing_thread_margin_not_met");
        else if (input->expected_gain_ms <= input->migration_cost_ms)
            finish(result, P5_OPPORTUNITY_NOT_BENEFICIAL, "expected_gain_does_not_exceed_calibrated_cost");
        else {
            result->roi_margin_ms = input->expected_gain_ms - input->migration_cost_ms;
            result->roi_available = true;
            finish(result, P5_OPPORTUNITY_BENEFICIAL, "candidate_bound_evidence_exceeds_calibrated_cost");
        }
    }
    return 0;
}

bool p5_opportunity_calibration_evidence(const CalibrationSnapshot *snapshot,
                                         const CalibrationMatchRequest *request,
                                         double *expected_gain_pct,
                                          double *migration_cost_ms,
                                         calibration_match_status_t *status)
{
    ValidatedCalibrationMatch match;
    calibration_match_status_t matched;

    if (expected_gain_pct != NULL) *expected_gain_pct = 0.0;
    if (migration_cost_ms != NULL) *migration_cost_ms = 0.0;
    matched = calibration_match(snapshot, request, &match);
    if (status != NULL) *status = matched;
    if (matched != CALIBRATION_MATCHED || expected_gain_pct == NULL || migration_cost_ms == NULL ||
        !isfinite(match.expected_gain_pct) || !isfinite(match.migration_cost_conservative_ms))
        return false;
    *expected_gain_pct = match.expected_gain_pct;
    *migration_cost_ms = match.migration_cost_conservative_ms;
    return true;
}

int p5_opportunity_write_state(const char *path, const p5_opportunity_result_t *result)
{
    char temporary[4096];
    int descriptor;
    FILE *file;
    if (path == NULL || result == NULL ||
        snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary))
        return -1;
    descriptor = mkstemp(temporary);
    if (descriptor < 0 || (file = fdopen(descriptor, "w")) == NULL) {
        if (descriptor >= 0) close(descriptor);
        return -1;
    }
    fprintf(file, "schema_version,app_id,pid,start_time_ticks,temporal_generation,candidate_tid,candidate_tid_start_time_ticks,classification,classification_available,placement_relation,placement_available,source_cpu,source_node,memory_total_pages,memory_queryable_pages,memory_dominant_node,proposed_cpu,proposed_node,destination_available,expected_gain_ms,expected_gain_available,migration_cost_ms,migration_cost_available,roi_margin_ms,roi_available,decision_score,decision_available,opportunity_status,beneficial,reason\n");
    fprintf(file, "2,%s,%ld,%llu,%llu,%ld,%llu,%s,%s,%s,%s,%d,%d,%zu,%zu,%d,%d,%d,%s,%.9g,%s,%.9g,%s,%.9g,%s,%.9g,%s,%s,%s,%s\n",
            result->app_id, (long)result->pid, (unsigned long long)result->start_time_ticks,
            (unsigned long long)result->temporal_generation, (long)result->candidate_tid,
            (unsigned long long)result->candidate_tid_start_time_ticks, result->classification,
            result->classification_available ? "true" : "false", result->placement_relation,
             result->placement_available ? "true" : "false", result->source_cpu, result->source_node,
             result->memory_total_pages, result->memory_queryable_pages, result->memory_dominant_node,
             result->proposed_cpu, result->proposed_node, result->destination_available ? "true" : "false",
             result->expected_gain_ms, result->expected_gain_available ? "true" : "false",
             result->migration_cost_ms, result->migration_cost_available ? "true" : "false",
             result->roi_margin_ms, result->roi_available ? "true" : "false",
            result->decision_score, result->decision_available ? "true" : "false",
            p5_opportunity_status_name(result->status), result->beneficial ? "true" : "false", result->reason);
    if (fflush(file) != 0 || fsync(descriptor) != 0 || fclose(file) != 0 || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}
