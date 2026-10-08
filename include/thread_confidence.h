#ifndef AWAVMA_THREAD_CONFIDENCE_H
#define AWAVMA_THREAD_CONFIDENCE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define THREAD_CONFIDENCE_REQUIRED_STREAK 3U

typedef enum {
    THREAD_CONFIDENCE_PASS,
    THREAD_CONFIDENCE_INSUFFICIENT_STREAK,
    THREAD_CONFIDENCE_INSUFFICIENT_EVIDENCE,
    THREAD_CONFIDENCE_CANDIDATE_UNAVAILABLE,
    THREAD_CONFIDENCE_GENERATION_GAP,
    THREAD_CONFIDENCE_INVALID_EVIDENCE,
    THREAD_CONFIDENCE_IDENTITY_MISMATCH,
    THREAD_CONFIDENCE_HISTORY_UNAVAILABLE,
    THREAD_CONFIDENCE_MALFORMED_HISTORY,
    THREAD_CONFIDENCE_UNSUPPORTED_SCHEMA,
    THREAD_CONFIDENCE_DUPLICATE_CONFLICT
} thread_confidence_status_t;

typedef struct {
    char app_id[128];
    pid_t pid;
    uint64_t start_time_ticks;
    pid_t candidate_tid;
    thread_confidence_status_t status;
    bool available;
    bool qualified;
    uint64_t streak_length;
    uint64_t required_streak;
    uint64_t latest_temporal_generation;
    char reason[128];
} thread_confidence_result_t;

const char *thread_confidence_status_name(thread_confidence_status_t status);
int thread_confidence_evaluate(const char *history_path, const char *app_id, pid_t pid,
                               uint64_t start_time_ticks, pid_t candidate_tid,
                               thread_confidence_result_t *result);
int thread_confidence_write_state(const char *path, const thread_confidence_result_t *result);

#endif
