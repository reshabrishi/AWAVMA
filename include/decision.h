#ifndef AWAVMA_DECISION_H
#define AWAVMA_DECISION_H

#include <stdbool.h>
#include <stddef.h>

#define DECISION_FACTOR_COUNT 6U
#define DECISION_MAX_WEIGHT_OVERRIDES 12U

typedef enum {
    DECISION_MEMORY,
    DECISION_THREAD
} decision_action_t;

typedef enum {
    FACTOR_ACCESS,
    FACTOR_THRESHOLD,
    FACTOR_GAIN,
    FACTOR_COST,
    FACTOR_CPU,
    FACTOR_SHARING
} decision_factor_t;

typedef struct {
    decision_action_t action;
    decision_factor_t factor;
    double value;
} decision_weight_override_t;

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *state_dir;
    const char *history_dir;
    const char *log_path;
    const char *app_id;
    double epsilon;
    double bias_min;
    double bias_max;
    double default_memory_bias;
    double default_thread_bias;
    bool override_bias;
    decision_weight_override_t weight_overrides[DECISION_MAX_WEIGHT_OVERRIDES];
    size_t weight_override_count;
    size_t history_max_records;
    double history_max_days;
    double history_decay_lambda;
    size_t cleanup_interval;
    double inactive_days;
    double expire_days;
} decision_config_t;

typedef struct {
    size_t input_rows;
    size_t valid_decisions;
    size_t insufficient_rows;
    size_t move_memory;
    size_t move_thread;
    size_t no_migration;
    size_t applications;
    double runtime_seconds;
} decision_summary_t;

typedef struct {
    double memory_delta[DECISION_FACTOR_COUNT];
    double thread_delta[DECISION_FACTOR_COUNT];
    double memory_bias_delta;
    double thread_bias_delta;
} decision_feedback_update_t;

typedef struct {
    double old_memory_weights[DECISION_FACTOR_COUNT];
    double new_memory_weights[DECISION_FACTOR_COUNT];
    double old_thread_weights[DECISION_FACTOR_COUNT];
    double new_thread_weights[DECISION_FACTOR_COUNT];
    double old_memory_bias;
    double new_memory_bias;
    double old_thread_bias;
    double new_thread_bias;
    unsigned weight_version;
    unsigned bias_version;
} decision_feedback_snapshot_t;

const char *decision_action_name(decision_action_t action);
const char *decision_factor_name(decision_factor_t factor);
int decision_run(const decision_config_t *config, decision_summary_t *summary);
int decision_feedback_apply(const decision_config_t *config, const char *app_id,
                            const decision_feedback_update_t *update,
                            double min_weight, double max_weight,
                            decision_feedback_snapshot_t *snapshot);

#endif
