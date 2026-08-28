#ifndef AWAVMA_WORKER_TYPES_H
#define AWAVMA_WORKER_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*worker_job_fn)(void *arg);

typedef struct {
    worker_job_fn function;
    void *arg;
    uint64_t job_id;
} worker_job_t;

typedef struct {
    unsigned worker_id;
    bool busy;
    uint64_t current_job_id;
    uint64_t jobs_completed;
} worker_stats_t;

#endif
