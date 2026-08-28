#include "safety.h"

#include <math.h>
#include <stdio.h>

static GateResult invalid_result(const char *reason)
{
    GateResult result = {.status = GATE_INVALID, .score = -1.0};
    snprintf(result.reason, sizeof(result.reason), "%s", reason);
    return result;
}

GateResult EvaluateSafetyGate(const MonitorData *monitor,
                              const DecisionData *decision,
                              const ValidationConfig *config)
{
    GateResult result;
    double cpu_value;
    double memory_value;
    double score;

    if (monitor == NULL || decision == NULL || config == NULL ||
        !isfinite(config->cpu_weight) || !isfinite(config->memory_weight) ||
        !isfinite(config->concurrency_weight) || !isfinite(config->safety_threshold) ||
        config->cpu_weight < 0.0 || config->memory_weight < 0.0 ||
        config->concurrency_weight < 0.0)
        return invalid_result("REJECT_INVALID_INPUT");
    if (!monitor->available || !monitor->hard_constraints_available ||
        !isfinite(monitor->cpu_utilization) || !isfinite(monitor->memory_utilization) ||
        !isfinite(monitor->concurrency_score) || monitor->cpu_utilization < 0.0 ||
        monitor->cpu_utilization > 100.0 || monitor->memory_utilization < 0.0 ||
        monitor->memory_utilization > 100.0 || monitor->concurrency_score < 0.0 ||
        monitor->concurrency_score > 100.0)
        return invalid_result("REJECT_INSUFFICIENT_SIGNAL");

    /* Hard veto order is deterministic and cannot be compensated by Ssoft. */
    if (monitor->page_locked)
        return invalid_result("REJECT_PAGE_LOCKED");
    if (monitor->memory_pinned)
        return invalid_result("REJECT_MEMORY_PINNED");
    if (monitor->cooldown_active)
        return invalid_result("REJECT_COOLDOWN");
    if (monitor->thread_locked)
        return invalid_result("REJECT_THREAD_LOCKED");
    if (monitor->migration_in_progress)
        return invalid_result("REJECT_MIGRATION_IN_PROGRESS");
    if (monitor->max_migrations_reached)
        return invalid_result("REJECT_MAX_MIGRATIONS");

    cpu_value = 100.0 - monitor->cpu_utilization;
    memory_value = 100.0 - monitor->memory_utilization;
    score = config->cpu_weight * cpu_value + config->memory_weight * memory_value +
            config->concurrency_weight * monitor->concurrency_score;
    result.status = score >= config->safety_threshold ? GATE_PASS : GATE_FAIL;
    result.score = score;
    snprintf(result.reason, sizeof(result.reason), "%s", result.status == GATE_PASS ? "PASS" : "REJECT_UNSAFE");
    return result;
}
