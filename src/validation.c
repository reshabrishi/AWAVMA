#include "validation.h"
#include "validation_log.h"
#include "monitor_profile.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static ValidationConfig active_config;
static bool initialized;

static bool approximately_one(double value)
{
    return fabs(value - 1.0) <= 0.0001;
}

static bool valid_config(const ValidationConfig *config)
{
    if (config == NULL || !isfinite(config->stability_weight) || !isfinite(config->sample_weight) ||
        !isfinite(config->classifier_weight) || !isfinite(config->gain_weight) ||
        !isfinite(config->cost_weight) || !isfinite(config->cpu_weight) ||
        !isfinite(config->memory_weight) || !isfinite(config->concurrency_weight) ||
        !isfinite(config->confidence_weight) || !isfinite(config->roi_weight) ||
        !isfinite(config->safety_weight) || !isfinite(config->confidence_threshold) ||
        !isfinite(config->roi_threshold) || !isfinite(config->safety_threshold) ||
        !isfinite(config->validation_threshold) || !isfinite(config->history_max_days) ||
        !isfinite(config->history_decay_lambda) || config->sample_reference_count == 0 ||
        config->history_max_records == 0 || config->history_max_days <= 0.0 ||
        config->history_cleanup_interval == 0 || config->history_decay_lambda <= 0.0)
        return false;
    if (config->stability_weight < 0.0 || config->sample_weight < 0.0 || config->classifier_weight < 0.0 ||
        config->gain_weight < 0.0 || config->cost_weight < 0.0 || config->cpu_weight < 0.0 ||
        config->memory_weight < 0.0 || config->concurrency_weight < 0.0 || config->confidence_weight < 0.0 ||
        config->roi_weight < 0.0 || config->safety_weight < 0.0)
        return false;
    if (!approximately_one(config->stability_weight + config->sample_weight + config->classifier_weight) ||
        !approximately_one(config->gain_weight + config->cost_weight) ||
        !approximately_one(config->cpu_weight + config->memory_weight + config->concurrency_weight) ||
        !approximately_one(config->confidence_weight + config->roi_weight + config->safety_weight))
        return false;
    return config->confidence_threshold >= 0.0 && config->confidence_threshold <= 100.0 &&
           config->roi_threshold >= -100.0 && config->roi_threshold <= 100.0 &&
           config->safety_threshold >= 0.0 && config->safety_threshold <= 100.0 &&
           config->validation_threshold >= -100.0 && config->validation_threshold <= 100.0;
}

bool Validation_Init(const ValidationConfig *config)
{
    if (!valid_config(config))
        return false;
    active_config = *config;
    initialized = true;
    if (!validation_log_init(config)) {
        initialized = false;
        return false;
    }
    return true;
}

static void copy_identity(ValidationResult *result, const DecisionData *decision)
{
    snprintf(result->migration_id, sizeof(result->migration_id), "%s", decision->migration_id);
    snprintf(result->app_id, sizeof(result->app_id), "%s", decision->app_id);
    snprintf(result->entity_id, sizeof(result->entity_id), "%s", decision->entity_id);
    result->pid = decision->pid;
    result->action = decision->action;
}

static ValidationResult base_result(const DecisionData *decision)
{
    ValidationResult result;

    memset(&result, 0, sizeof(result));
    result.confidence_score = -1.0;
    result.roi_score = -1.0;
    result.safety_score = -1.0;
    result.validation_score = -1.0;
    snprintf(result.timestamp, sizeof(result.timestamp), "UNKNOWN");
    copy_identity(&result, decision);
    result.confidence_status = GATE_INVALID;
    result.roi_status = GATE_INVALID;
    result.safety_status = GATE_INVALID;
    return result;
}

ValidationResult CalculateValidationScore(const GateResult *confidence,
                                          const GateResult *roi,
                                          const GateResult *safety,
                                          const ValidationConfig *config)
{
    ValidationResult result;

    memset(&result, 0, sizeof(result));
    result.confidence_status = GATE_INVALID;
    result.roi_status = GATE_INVALID;
    result.safety_status = GATE_INVALID;
    result.confidence_score = -1.0;
    result.roi_score = -1.0;
    result.safety_score = -1.0;
    result.validation_score = -1.0;
    if (confidence == NULL || roi == NULL || safety == NULL || config == NULL) {
        snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_INVALID_INPUT");
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    if (!isfinite(config->confidence_weight) || !isfinite(config->roi_weight) ||
        !isfinite(config->safety_weight) || !isfinite(config->validation_threshold) ||
        config->confidence_weight < 0.0 || config->roi_weight < 0.0 || config->safety_weight < 0.0) {
        snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_INVALID_INPUT");
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    result.confidence_score = confidence->score;
    result.roi_score = roi->score;
    result.safety_score = safety->score;
    result.confidence_status = confidence->status;
    result.roi_status = roi->status;
    result.safety_status = safety->status;
    if (confidence->status != GATE_PASS || roi->status != GATE_PASS || safety->status != GATE_PASS) {
        const char *reason = confidence->status == GATE_INVALID ? "REJECT_INVALID_INPUT" :
                             confidence->status == GATE_FAIL ? "REJECT_LOW_CONFIDENCE" :
                             roi->status == GATE_INVALID ? "REJECT_INVALID_INPUT" :
                             roi->status == GATE_FAIL ? "REJECT_LOW_ROI" :
                             safety->status == GATE_INVALID ? "REJECT_INVALID_INPUT" : "REJECT_UNSAFE";
        snprintf(result.validation_status, sizeof(result.validation_status), "%s",
                 reason);
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    result.validation_score = config->confidence_weight * confidence->score +
                              config->roi_weight * roi->score +
                              config->safety_weight * safety->score;
    snprintf(result.validation_status, sizeof(result.validation_status), "%s",
             result.validation_score >= config->validation_threshold ? "PASS" : "REJECT_LOW_VALIDATION_SCORE");
    snprintf(result.final_decision, sizeof(result.final_decision), "%s",
             result.validation_score >= config->validation_threshold ? "APPROVED" : "REJECTED");
    return result;
}

ValidationResult ValidateMigration(const MonitorData *monitor,
                                   const DecisionData *decision,
                                   const ClassifierData *classifier)
{
    ValidationResult result;
    GateResult confidence;
    GateResult roi;
    GateResult safety;
    monitor_profile_scope_t confidence_profile;
    monitor_profile_scope_t roi_profile;
    monitor_profile_scope_t safety_profile;

    if (!initialized || decision == NULL) {
        memset(&result, 0, sizeof(result));
        result.confidence_status = GATE_INVALID;
        result.roi_status = GATE_INVALID;
        result.safety_status = GATE_INVALID;
        result.confidence_score = -1.0;
        result.roi_score = -1.0;
        result.safety_score = -1.0;
        result.validation_score = -1.0;
        snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_INVALID_INPUT");
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    result = base_result(decision);
    if (decision->action == VALIDATION_ACTION_NO_MIGRATION) {
        snprintf(result.validation_status, sizeof(result.validation_status), "VALID_NO_MIGRATION");
        snprintf(result.final_decision, sizeof(result.final_decision), "NO_MIGRATION");
        return result;
    }
    if (decision->action == VALIDATION_ACTION_INSUFFICIENT) {
        snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_INSUFFICIENT_SIGNAL");
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    if (decision->action != VALIDATION_ACTION_MOVE_MEMORY && decision->action != VALIDATION_ACTION_MOVE_THREAD) {
        snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_INVALID_INPUT");
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    monitor_profile_scope_begin(&confidence_profile, "phase56_child", "p6_confidence_gate",
                                decision->app_id, decision->pid, 0);
    confidence = EvaluateConfidenceGate(monitor, classifier, &active_config);
    monitor_profile_scope_end(&confidence_profile, "OK");
    monitor_profile_scope_begin(&roi_profile, "phase56_child", "p6_roi_gate",
                                decision->app_id, decision->pid, 0);
    roi = EvaluateROIGate(decision, &active_config);
    monitor_profile_scope_end(&roi_profile, "OK");
    monitor_profile_scope_begin(&safety_profile, "phase56_child", "p6_safety_gate",
                                decision->app_id, decision->pid, 0);
    safety = EvaluateSafetyGate(monitor, decision, &active_config);
    monitor_profile_scope_end(&safety_profile, "OK");
    result.confidence_score = confidence.score;
    result.roi_score = roi.score;
    result.safety_score = safety.score;
    result.confidence_status = confidence.status;
    result.roi_status = roi.status;
    result.safety_status = safety.status;
    if (confidence.status == GATE_INVALID || roi.status == GATE_INVALID || safety.status == GATE_INVALID) {
        if (strcmp(safety.reason, "REJECT_PAGE_LOCKED") == 0 ||
            strcmp(safety.reason, "REJECT_MEMORY_PINNED") == 0 ||
            strcmp(safety.reason, "REJECT_COOLDOWN") == 0 ||
            strcmp(safety.reason, "REJECT_THREAD_LOCKED") == 0 ||
            strcmp(safety.reason, "REJECT_MIGRATION_IN_PROGRESS") == 0 ||
            strcmp(safety.reason, "REJECT_MAX_MIGRATIONS") == 0) {
            snprintf(result.validation_status, sizeof(result.validation_status), "%s", safety.reason);
        } else if (strcmp(confidence.reason, "REJECT_INSUFFICIENT_SIGNAL") == 0 ||
                   strcmp(roi.reason, "REJECT_INSUFFICIENT_SIGNAL") == 0 ||
                   strcmp(safety.reason, "REJECT_INSUFFICIENT_SIGNAL") == 0) {
            snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_INSUFFICIENT_SIGNAL");
        } else {
            snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_INVALID_INPUT");
        }
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    result.validation_score = active_config.confidence_weight * confidence.score +
                              active_config.roi_weight * roi.score +
                              active_config.safety_weight * safety.score;
    if (confidence.status == GATE_FAIL) {
        snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_LOW_CONFIDENCE");
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    if (roi.status == GATE_FAIL) {
        snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_LOW_ROI");
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    if (safety.status == GATE_FAIL) {
        snprintf(result.validation_status, sizeof(result.validation_status), "REJECT_UNSAFE");
        snprintf(result.final_decision, sizeof(result.final_decision), "REJECTED");
        return result;
    }
    result = CalculateValidationScore(&confidence, &roi, &safety, &active_config);
    copy_identity(&result, decision);
    return result;
}

void Validation_Shutdown(void)
{
    validation_log_shutdown();
    initialized = false;
    memset(&active_config, 0, sizeof(active_config));
}
