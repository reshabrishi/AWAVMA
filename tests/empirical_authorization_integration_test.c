#include "calibration.h"
#include "empirical_memory_decision.h"
#include "migration_safety_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { unsigned attempts, checkpoints, benefits, executors, feedbacks; } Fixture;

static bool identity(void *ctx, pid_t pid, uint64_t ticks)
{ (void)ctx; return pid == 4242 && ticks == 99; }

static MigrationTargetResult target(void *ctx, const MigrationSafetyRequest *r, const char *id,
                                    MigrationTarget *out)
{
    (void)ctx; memset(out, 0, sizeof(*out)); out->pid = r->pid; out->start_time_ticks = r->start_time_ticks;
    out->action = r->action; out->source_node_known = true; out->source_numa_node = 1;
    out->has_target_numa_node = true; out->target_numa_node = 0;
    snprintf(out->attempt_id, sizeof(out->attempt_id), "%s", id); return MIGRATION_TARGET_AVAILABLE;
}

/* The sole seam provides the attempt-bound checkpoint, avoiding any move_pages syscall. */
static PageCheckpointResult checkpoint(void *ctx, const MigrationSafetyRequest *r, const char *id,
                                       MigrationPageCheckpoint *out)
{
    Fixture *f = ctx; f->checkpoints++; out->entries = calloc(1, sizeof(*out->entries));
    if (out->entries == NULL) return PAGE_CHECKPOINT_ALLOCATION_FAILED;
    out->pid = r->pid; out->start_time_ticks = r->start_time_ticks; out->requested_count = out->known_count = 1;
    out->complete = true; out->entries[0].original_node_known = true; out->entries[0].original_node = 1;
    snprintf(out->attempt_id, sizeof(out->attempt_id), "%s", id); return PAGE_CHECKPOINT_COMPLETE;
}

static BenefitClassification benefit(void *ctx, const MigrationSafetyRequest *r, const MigrationTarget *t,
                                      const char *id, const MemoryRecoveryEvidence *recovery, BenefitDecision *out)
{
    Fixture *f = ctx; BenefitClassifierInput in = {0}; f->benefits++;
    in.pid = r->pid; in.start_time_ticks = r->start_time_ticks; in.attempt_id = id; in.action = r->action;
    in.process_active = in.identity_match = in.phase5_fresh = in.phase6_fresh = true;
    in.evidence_attempt_bound = r->migration_request.benefit_evidence_bound;
    in.decision = &r->migration_request.phase5_decision; in.validation = &r->migration_request.phase6_validation;
    in.target = t; in.memory_recovery = recovery; in.memory_candidate_count = r->migration_request.page_count;
    in.target_provider_validated = in.target_online = in.target_permitted = true;
    in.source_known = in.source_target_valid = true;
    in.calibration.state = BENEFIT_CALIBRATION_VALIDATED_PRODUCTION;
    in.calibration.provenance = "validated-production-fixture";
    return benefit_classifier_evaluate(&in, out);
}

static MigrationResultCode execute(void *ctx, const MigrationRequest *r, MigrationReport *report)
{ (void)r; (void)report; ((Fixture *)ctx)->executors++; return MIGRATION_SYSTEM_ERROR; }
static MigrationSafetyRecovery rollback(void *ctx, const MigrationSafetyRequest *r, const MigrationReport *report)
{ (void)ctx; (void)r; (void)report; return MIGRATION_SAFETY_ROLLBACK_UNAVAILABLE; }
static bool feedback(void *ctx, const FeedbackEvent *event, FeedbackResult *out)
{ (void)event; ((Fixture *)ctx)->feedbacks++; memset(out, 0, sizeof(*out)); return true; }

static bool match(double cost, ValidatedCalibrationMatch *out)
{
    CalibrationRecord r = {0}; CalibrationSnapshot snapshot = {0}; CalibrationMatchRequest request = {0};
    calibration_match_status_t status; char reason[CALIBRATION_REASON_MAX]; char path[128] = ""; bool matched;
    r.schema_version = CALIBRATION_SCHEMA_VERSION; r.status = CALIBRATION_VALIDATED_PRODUCTION;
    r.action = VALIDATION_ACTION_MOVE_MEMORY; r.source_node = 1; r.destination_node = 0;
    r.migration_page_bucket = 1; r.valid_pair_count = r.cost_sample_count = 7;
    r.compatibility.online_numa_nodes = 2; r.compatibility.local_node = 0; r.compatibility.remote_node = 1;
    r.compatibility.numa_distance = 20; r.compatibility.local_permitted_cpu_count = 1;
    r.compatibility.page_size_bytes = 4096; r.workload.threads = 1; r.workload.memory_bytes = 4096;
    r.workload.memory_pages = 1; r.workload.duration_seconds = 1.0; r.local_mean_ms = 80.0;
    r.remote_mean_ms = 100.0; r.paired_penalty_mean_ms = r.paired_penalty_lower_bound_ms = 20.0;
    r.expected_recoverable_gain_pct = 20.0; r.migration_cost_mean_ms = r.migration_cost_conservative_ms = cost;
    r.estimated_cost_pct = cost; r.successful_pages = 1; r.placement_evidence_schema_version = 1;
    snprintf(r.calibration_version, sizeof(r.calibration_version), "p4c-v2");
    snprintf(r.collection_experiment_id, sizeof(r.collection_experiment_id), "blocker6");
    snprintf(r.created_at_utc, sizeof(r.created_at_utc), "2026-10-10T00:00:00Z");
    snprintf(r.compatibility.cpu_architecture, sizeof(r.compatibility.cpu_architecture), "test");
    snprintf(r.compatibility.cpu_model, sizeof(r.compatibility.cpu_model), "test");
    snprintf(r.workload.benchmark_pattern, sizeof(r.workload.benchmark_pattern), "stream");
    snprintf(r.local_placement_artifact_hash, sizeof(r.local_placement_artifact_hash), "art1-0000000000000000");
    snprintf(r.remote_placement_artifact_hash, sizeof(r.remote_placement_artifact_hash), "art1-1111111111111111");
    snprintf(r.migration_measurement_method, sizeof(r.migration_measurement_method), "move_pages");
    snprintf(r.rejection_reason, sizeof(r.rejection_reason), "none");
    if (!calibration_topology_fingerprint(&r.compatibility, r.compatibility.topology_fingerprint) ||
        !calibration_record_id(&r, r.calibration_id) ||
        snprintf(path, sizeof(path), "/tmp/awavma-b6-%ld.csv", (long)getpid()) >= (int)sizeof(path) ||
        calibration_write_csv(path, &r) != 0 || calibration_load_csv(path, NULL, &snapshot, &status, reason) != 0) {
        if (path[0] != '\0')
            unlink(path);
        return false;
    }
    request.action = r.action; request.source_node = r.source_node; request.destination_node = r.destination_node;
    request.migration_page_bucket = r.migration_page_bucket; request.compatibility = r.compatibility; request.workload = r.workload;
    matched = status == CALIBRATION_MATCHED && calibration_match(&snapshot, &request, out) == CALIBRATION_MATCHED;
    calibration_snapshot_release(&snapshot); unlink(path); return matched;
}

int main(void)
{
    EmpiricalMemoryDecisionFacts facts = {.requested_action = VALIDATION_ACTION_MOVE_MEMORY,
        .identity_authoritative = true, .identity_current = true, .registration_authoritative = true,
        .registration_current = true, .activity_authoritative = true, .activity_current = true,
        .placement_authoritative = true, .memory_is_remote = true, .destination_is_local = true,
        .source_node = 1, .destination_node = 0, .migration_page_bucket = 1};
    ValidatedCalibrationMatch positive_match, negative_match; DecisionData phase5; ValidationResult phase6 = {0};
    MigrationSafetyRequest request = {0}; MigrationSafetyResult result; MigrationSafetyConfig config;
    MigrationSafetyManager *manager; Fixture fixture = {0}; void *pages[] = {(void *)4096}; bool positive, negative;

    positive = match(5.0, &positive_match) && empirical_memory_decision(&positive_match, &facts, &phase5);
    phase5.pid = 4242; phase5.phase5_evidence_available = phase5.phase5_runtime_generation_available = true;
    phase5.phase5_runtime_generation = 1; snprintf(phase5.entity_id, sizeof(phase5.entity_id), "worker-1");
    snprintf(phase5.migration_id, sizeof(phase5.migration_id), "empirical-4242-1");
    phase6.pid = 4242; phase6.action = VALIDATION_ACTION_MOVE_MEMORY; phase6.confidence_status = phase6.roi_status = GATE_PASS;
    phase6.safety_status = GATE_NOT_APPLICABLE; phase6.confidence_score = 1.0; phase6.roi_score = phase5.empirical_roi;
    phase6.safety_score = -1.0; snprintf(phase6.migration_id, sizeof(phase6.migration_id), "%s", phase5.migration_id);
    snprintf(phase6.final_decision, sizeof(phase6.final_decision), "APPROVED");
    snprintf(request.app_id, sizeof(request.app_id), "blocker6"); request.pid = 4242; request.start_time_ticks = 99;
    request.action = VALIDATION_ACTION_MOVE_MEMORY; request.source_numa_node = 1; request.destination_numa_node = 0;
    request.placement_available = request.target_valid = request.system_safe = true;
    request.migration_request.pid = 4242; request.migration_request.start_time_ticks = 99;
    request.migration_request.start_time_ticks_available = true; request.migration_request.phase5_decision = phase5;
    request.migration_request.phase6_validation = phase6; request.migration_request.page_metadata_available = true;
    request.migration_request.page_addresses_authoritative = request.migration_request.memory_region_verified = true;
    request.migration_request.pages = pages; request.migration_request.page_count = 1;
    request.migration_request.benefit_evidence_runtime_generation = 1;
    migration_safety_config_default(&config); config.enabled = true; config.execution_enabled = false;
    config.identity_fn = identity; config.target_fn = target; config.page_checkpoint_fn = checkpoint;
    config.benefit_fn = benefit; config.execute_fn = execute; config.rollback_fn = rollback;
    config.feedback_fn = feedback; config.callback_context = &fixture; manager = migration_safety_manager_create();
    positive = positive && manager != NULL && migration_safety_manager_init(manager, &config) &&
        (++fixture.attempts, migration_safety_manager_attempt(manager, &request, &result)) &&
        result.state == MIGRATION_SAFETY_EXECUTION_DISABLED && result.benefit_result == BENEFIT_SUPPORTED &&
        fixture.checkpoints == 1 && fixture.benefits == 1 && fixture.executors == 0 && fixture.feedbacks == 1;
    printf("B6I01_EXACT_MATCH_EMPIRICAL_PHASE6_BENEFIT_EXECUTION_DISABLED: %s\n", positive ? "PASS" : "FAIL");
    migration_safety_manager_destroy(manager);
    negative = match(25.0, &negative_match) && !empirical_memory_decision(&negative_match, &facts, &phase5) &&
        phase5.empirical_roi_available && strcmp(phase5.status_text, "NOT_BENEFICIAL") == 0 &&
        fixture.attempts == 1 && fixture.checkpoints == 1 && fixture.benefits == 1 && fixture.executors == 0;
    printf("B6I02_EXACT_MATCH_NEGATIVE_ROI_NOT_BENEFICIAL_NO_PHASE7: %s\n", negative ? "PASS" : "FAIL");
    return positive && negative ? EXIT_SUCCESS : EXIT_FAILURE;
}
