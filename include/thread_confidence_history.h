#ifndef AWAVMA_THREAD_CONFIDENCE_HISTORY_H
#define AWAVMA_THREAD_CONFIDENCE_HISTORY_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    THREAD_CONFIDENCE_HISTORY_VALID,
    THREAD_CONFIDENCE_HISTORY_INVALID_EVIDENCE,
    THREAD_CONFIDENCE_HISTORY_CANDIDATE_UNAVAILABLE,
    THREAD_CONFIDENCE_HISTORY_CANDIDATE_VERIFIED,
    THREAD_CONFIDENCE_HISTORY_MALFORMED_INPUT,
    THREAD_CONFIDENCE_HISTORY_IDENTITY_MISMATCH,
    THREAD_CONFIDENCE_HISTORY_PERSISTENCE_UNAVAILABLE,
    THREAD_CONFIDENCE_HISTORY_UNSUPPORTED
} thread_confidence_history_status_t;

typedef struct {
    const char *timestamp_utc;
    const char *app_id;
    pid_t pid;
    uint64_t start_time_ticks;
    uint64_t temporal_generation;
    pid_t candidate_tid;
    bool candidate_tid_available;
    bool candidate_tid_verified;
    const char *classification;
    bool classification_available;
    const char *placement_relation;
    bool placement_available;
    thread_confidence_history_status_t evidence_status;
    bool observation_valid;
    const char *reason;
    const char *provenance;
} thread_confidence_history_observation_t;

const char *thread_confidence_history_status_name(thread_confidence_history_status_t status);
int thread_confidence_history_next_generation(const char *path, const char *app_id, pid_t pid,
                                              uint64_t start_time_ticks, uint64_t *generation);
int thread_confidence_history_persist(const char *path,
                                      const thread_confidence_history_observation_t *observation);

#endif
