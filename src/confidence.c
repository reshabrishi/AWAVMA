#include "confidence.h"

#include <math.h>
#include <stdio.h>

static GateResult invalid_result(const char *reason)
{
    GateResult result = {.status = GATE_INVALID, .score = -1.0};
    snprintf(result.reason, sizeof(result.reason), "%s", reason);
    return result;
}

GateResult EvaluateConfidenceGate(const MonitorData *monitor,
                                  const ClassifierData *classifier,
                                  const ValidationConfig *config)
{
    GateResult result;
    double sample_value;
    double score;

    if (monitor == NULL || classifier == NULL || config == NULL ||
        !isfinite(config->stability_weight) || !isfinite(config->sample_weight) ||
        !isfinite(config->classifier_weight) || !isfinite(config->confidence_threshold) ||
        config->stability_weight < 0.0 || config->sample_weight < 0.0 ||
        config->classifier_weight < 0.0)
        return invalid_result("REJECT_INVALID_INPUT");
    if (!monitor->available || !classifier->available || config->sample_reference_count == 0 ||
        !isfinite(monitor->stability) || !isfinite(classifier->confidence) ||
        monitor->stability < 0.0 || monitor->stability > 100.0 ||
        classifier->confidence < 0.0 || classifier->confidence > 100.0)
        return invalid_result("REJECT_INSUFFICIENT_SIGNAL");
    sample_value = (double)monitor->sample_count / (double)config->sample_reference_count;
    if (sample_value > 1.0)
        sample_value = 1.0;
    score = config->stability_weight * monitor->stability +
            config->sample_weight * sample_value * 100.0 +
            config->classifier_weight * classifier->confidence;
    result.status = score >= config->confidence_threshold ? GATE_PASS : GATE_FAIL;
    result.score = score;
    snprintf(result.reason, sizeof(result.reason), "%s", result.status == GATE_PASS ? "PASS" : "REJECT_LOW_CONFIDENCE");
    return result;
}
