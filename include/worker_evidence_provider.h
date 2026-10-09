#ifndef AWAVMA_WORKER_EVIDENCE_PROVIDER_H
#define AWAVMA_WORKER_EVIDENCE_PROVIDER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define WORKER_EVIDENCE_WIRE_VERSION 1U
#define WORKER_EVIDENCE_APP_ID_MAX 128U
#define WORKER_EVIDENCE_MAX_WORKERS 256U
#define WORKER_EVIDENCE_FRESHNESS_MS 3000U

typedef struct {
    uint32_t version;
    uint32_t operation;
    int32_t pid;
    int32_t tid;
    uint64_t process_start_time_ticks;
    uint64_t worker_start_time_ticks;
    uint64_t registration_generation;
    uint64_t evidence_generation;
    uint64_t interval_ms;
    uint64_t registered_memory_load_operations;
    uint32_t worker_index;
    char app_id[WORKER_EVIDENCE_APP_ID_MAX];
} WorkerEvidenceWireMessage;

typedef struct {
    char app_id[WORKER_EVIDENCE_APP_ID_MAX];
    pid_t pid;
    uint64_t process_start_time_ticks;
    pid_t tid;
    uint64_t worker_start_time_ticks;
    uint64_t registration_generation;
    uint64_t evidence_generation;
    uint64_t interval_ms;
    uint64_t load_operations_delta;
    uint32_t worker_index;
    uint64_t registered_memory_load_operations;
    uint64_t received_at_ms;
} worker_evidence_activity_t;

typedef struct worker_evidence_provider worker_evidence_provider_t;

worker_evidence_provider_t *worker_evidence_provider_create(void);
void worker_evidence_provider_destroy(worker_evidence_provider_t *provider);
bool worker_evidence_provider_start(worker_evidence_provider_t *provider, const char *socket_path);
void worker_evidence_provider_stop(worker_evidence_provider_t *provider);
bool worker_evidence_provider_poll(worker_evidence_provider_t *provider);
bool worker_evidence_provider_publish(const char *socket_path, const WorkerEvidenceWireMessage *message);
bool worker_evidence_provider_candidate(worker_evidence_provider_t *provider, const char *app_id,
                                        pid_t pid, uint64_t process_start_time_ticks, pid_t tid,
                                         uint64_t worker_start_time_ticks,
                                         uint64_t registration_generation,
                                          worker_evidence_activity_t *activity);
/* Copies only fresh, already authenticated activity for one collector-owned process. */
size_t worker_evidence_provider_snapshot(worker_evidence_provider_t *provider, pid_t pid,
                                         worker_evidence_activity_t *activities, size_t capacity);
int worker_evidence_activity_append(const char *path, const worker_evidence_activity_t *activity,
                                    bool exact_candidate_match);

#endif
