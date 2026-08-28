#ifndef AWAVMA_FEEDBACK_H
#define AWAVMA_FEEDBACK_H

#include "feedback_types.h"

#include <stdbool.h>

typedef struct {
    const char *state_dir;
    const char *history_dir;
    const char *results_path;
    const char *history_path;
    const char *log_path;
    double learning_rate;
    double bias_learning_rate;
    size_t min_samples;
    double deadband;
    double reward_min;
    double reward_max;
    double decay_lambda;
    double min_confidence;
    size_t history_max_records;
    double history_max_days;
    size_t cleanup_interval;
    double min_weight;
    double max_weight;
    double min_bias;
    double max_bias;
    double throughput_weight;
    double execution_time_weight;
    double latency_weight;
    double page_fault_weight;
} FeedbackConfig;

const char *FeedbackClassName(FeedbackClass feedback_class);
const char *FeedbackStatusName(FeedbackUpdateStatus status);
bool Feedback_Init(const FeedbackConfig *config);
FeedbackUpdateStatus ProcessFeedback(const FeedbackEvent *event,
                                     FeedbackResult *result);
bool Feedback_CleanupHistory(void);
void Feedback_Shutdown(void);

#endif
