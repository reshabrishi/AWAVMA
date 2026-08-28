#ifndef AWAVMA_WORKER_POOL_H
#define AWAVMA_WORKER_POOL_H

#include "worker_types.h"

#include <stddef.h>

#define WORKER_POOL_MAX_WORKERS 128U
#define WORKER_POOL_MAX_QUEUE_CAPACITY 1048576U

typedef enum {
    WORKER_POOL_SUCCESS = 0,
    WORKER_POOL_INVALID_ARGUMENT,
    WORKER_POOL_ALREADY_INITIALIZED,
    WORKER_POOL_NOT_INITIALIZED,
    WORKER_POOL_QUEUE_FULL,
    WORKER_POOL_SHUTDOWN_IN_PROGRESS,
    WORKER_POOL_THREAD_CREATE_FAILED,
    WORKER_POOL_INTERNAL_ERROR
} worker_pool_status_t;

typedef struct {
    size_t worker_count;
    size_t queue_capacity;
    const char *log_path;
} worker_pool_config_t;

typedef struct worker_pool worker_pool_t;

void worker_pool_config_default(worker_pool_config_t *config);
const char *worker_pool_status_name(worker_pool_status_t status);
worker_pool_t *worker_pool_create(void);
worker_pool_status_t worker_pool_init(worker_pool_t *pool,
                                      const worker_pool_config_t *config);
worker_pool_status_t worker_pool_submit(worker_pool_t *pool,
                                        const worker_job_t *job);
worker_pool_status_t worker_pool_wait_idle(worker_pool_t *pool);
worker_pool_status_t worker_pool_shutdown(worker_pool_t *pool);
worker_pool_status_t worker_pool_destroy(worker_pool_t *pool);
size_t worker_pool_queued(const worker_pool_t *pool);
size_t worker_pool_active(const worker_pool_t *pool);
size_t worker_pool_worker_count(const worker_pool_t *pool);
size_t worker_pool_queue_capacity(const worker_pool_t *pool);
size_t worker_pool_snapshot(const worker_pool_t *pool, worker_stats_t *stats,
                            size_t capacity);

#endif
