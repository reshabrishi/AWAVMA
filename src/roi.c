#include "roi.h"

#include <math.h>
#include <stdio.h>

GateResult EvaluateROIGate(const DecisionData *decision,
                           const ValidationConfig *config)
{
    GateResult result = {.status = GATE_INVALID, .score = -1.0};
    double score;

    if (decision == NULL || config == NULL || !isfinite(config->gain_weight) ||
        !isfinite(config->cost_weight) || !isfinite(config->roi_threshold) ||
        config->gain_weight < 0.0 || config->cost_weight < 0.0) {
        snprintf(result.reason, sizeof(result.reason), "REJECT_INVALID_INPUT");
        return result;
    }
    if (!decision->gain_available || !decision->cost_available ||
        !isfinite(decision->predicted_gain) || !isfinite(decision->estimated_cost) ||
        decision->predicted_gain < 0.0 || decision->predicted_gain > 100.0 ||
        decision->estimated_cost < 0.0 || decision->estimated_cost > 100.0) {
        snprintf(result.reason, sizeof(result.reason), "REJECT_INSUFFICIENT_SIGNAL");
        return result;
    }
    /* Raw ROI intentionally remains in approximately [-100, 100]. */
    score = config->gain_weight * decision->predicted_gain -
            config->cost_weight * decision->estimated_cost;
    result.status = score >= config->roi_threshold ? GATE_PASS : GATE_FAIL;
    result.score = score;
    snprintf(result.reason, sizeof(result.reason), "%s", result.status == GATE_PASS ? "PASS" : "REJECT_LOW_ROI");
    return result;
}
