#include "validation.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static ValidationConfig test_config(void)
{
    ValidationConfig config = {
        .stability_weight = 0.4,
        .sample_weight = 0.3,
        .classifier_weight = 0.3,
        .gain_weight = 0.6,
        .cost_weight = 0.4,
        .cpu_weight = 0.34,
        .memory_weight = 0.33,
        .concurrency_weight = 0.33,
        .confidence_weight = 0.30,
        .roi_weight = 0.40,
        .safety_weight = 0.30,
        .confidence_threshold = 70.0,
        .roi_threshold = 0.0,
        .safety_threshold = 50.0,
        .validation_threshold = 50.0,
        .sample_reference_count = 10,
        .history_max_days = 30.0,
        .history_max_records = 1000,
        .history_cleanup_interval = 100,
        .history_decay_lambda = 0.1,
        .results_path = "/tmp/awavma-validation-test-results.csv",
        .history_path = "/tmp/awavma-validation-test-history.csv",
        .log_path = "/tmp/awavma-validation-test.log"
    };
    return config;
}

static MonitorData test_monitor(void)
{
    return (MonitorData){
        .available = true,
        .stability = 90.0,
        .sample_count = 10,
        .cpu_utilization = 20.0,
        .memory_utilization = 30.0,
        .concurrency_score = 90.0,
        .hard_constraints_available = true
    };
}

static ClassifierData test_classifier(void)
{
    return (ClassifierData){.available = true, .confidence = 90.0};
}

static DecisionData test_decision(void)
{
    return (DecisionData){
        .action = VALIDATION_ACTION_MOVE_MEMORY,
        .predicted_gain = 80.0,
        .estimated_cost = 20.0,
        .gain_available = true,
        .cost_available = true
    };
}

int main(void)
{
    ValidationConfig config = test_config();
    MonitorData monitor = test_monitor();
    ClassifierData classifier = test_classifier();
    DecisionData decision = test_decision();
    GateResult confidence;
    GateResult roi;
    GateResult safety;
    ValidationResult validation;

    confidence = EvaluateConfidenceGate(&monitor, &classifier, &config);
    assert(confidence.status == GATE_PASS);
    assert(fabs(confidence.score - 93.0) < 0.000001);

    roi = EvaluateROIGate(&decision, &config);
    assert(roi.status == GATE_PASS);
    assert(fabs(roi.score - 40.0) < 0.000001);

    safety = EvaluateSafetyGate(&monitor, &decision, &config);
    assert(safety.status == GATE_PASS);
    assert(fabs(safety.score - 80.0) < 0.000001);

    validation = CalculateValidationScore(&confidence, &roi, &safety, &config);
    assert(strcmp(validation.final_decision, "APPROVED") == 0);
    assert(fabs(validation.validation_score - 67.9) < 0.000001);

    monitor.page_locked = true;
    safety = EvaluateSafetyGate(&monitor, &decision, &config);
    assert(safety.status == GATE_INVALID);
    assert(safety.score == -1.0);
    assert(strcmp(safety.reason, "REJECT_PAGE_LOCKED") == 0);

    monitor = test_monitor();
    classifier.available = false;
    confidence = EvaluateConfidenceGate(&monitor, &classifier, &config);
    assert(confidence.status == GATE_INVALID);
    assert(confidence.score == -1.0);

    config.validation_threshold = NAN;
    assert(!Validation_Init(&config));
    return 0;
}
