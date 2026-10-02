#ifndef AWAVMA_THREAD_CONFIDENCE_H
#define AWAVMA_THREAD_CONFIDENCE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    size_t valid_sample_count;
    size_t consecutive_hot_count;
    size_t consecutive_remote_count;
    char latest_classification[32];
    char latest_placement_relation[32];
    char reason[64];
} ThreadConfidence;

bool thread_confidence_append(const char *cycle_evidence_path, const char *history_path, long pid,
                              uint64_t start_time_ticks, uint64_t cycle_generation);
bool thread_confidence_evaluate(const char *history_path, long pid, uint64_t start_time_ticks,
                                pid_t tid, ThreadConfidence *confidence);

#endif
