#define _POSIX_C_SOURCE 200809L

#include "worker_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct worker_pool {
    pthread_t *threads;
    worker_stats_t *stats;
    worker_job_t *jobs;
    size_t worker_count;
    size_t queue_capacity;
    size_t head;
    size_t tail;
    size_t queued;
    size_t active;
    bool accepting;
    bool stopping;
    bool initialized;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t idle;
    FILE *log_file;
};

typedef struct {
    worker_pool_t *pool;
    unsigned worker_id;
} worker_argument_t;

static void log_locked(worker_pool_t *pool, unsigned worker_id,
                       const char *event, uint64_t job_id)
{
    if (pool->log_file == NULL)
        return;
    fprintf(pool->log_file, "%lld,worker=%u,event=%s,job_id=%llu\n",
            (long long)time(NULL), worker_id, event,
            (unsigned long long)job_id);
    fflush(pool->log_file);
}

static void destroy_resources(worker_pool_t *pool)
{
    if (pool->log_file != NULL) {
        fclose(pool->log_file);
        pool->log_file = NULL;
    }
    pthread_cond_destroy(&pool->idle);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->threads);
    free(pool->stats);
    free(pool->jobs);
    pool->threads = NULL;
    pool->stats = NULL;
    pool->jobs = NULL;
    pool->worker_count = 0;
    pool->queue_capacity = 0;
    pool->head = 0;
    pool->tail = 0;
    pool->queued = 0;
    pool->active = 0;
    pool->accepting = false;
    pool->stopping = false;
    pool->initialized = false;
}

static void *worker_main(void *argument)
{
    worker_argument_t *worker = argument;
    worker_pool_t *pool = worker->pool;
    unsigned worker_id = worker->worker_id;

    free(worker);
    pthread_mutex_lock(&pool->mutex);
    log_locked(pool, worker_id, "worker_started", 0);
    pthread_mutex_unlock(&pool->mutex);
    for (;;) {
        worker_job_t job;

        pthread_mutex_lock(&pool->mutex);
        while (pool->queued == 0 && !pool->stopping)
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        if (pool->queued == 0 && pool->stopping) {
            log_locked(pool, worker_id, "worker_exited", 0);
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        job = pool->jobs[pool->head];
        pool->head = (pool->head + 1) % pool->queue_capacity;
        pool->queued--;
        pool->active++;
        pool->stats[worker_id].busy = true;
        pool->stats[worker_id].current_job_id = job.job_id;
        log_locked(pool, worker_id, "job_started", job.job_id);
        pthread_mutex_unlock(&pool->mutex);

        job.function(job.arg);

        pthread_mutex_lock(&pool->mutex);
        pool->active--;
        pool->stats[worker_id].busy = false;
        pool->stats[worker_id].current_job_id = 0;
        pool->stats[worker_id].jobs_completed++;
        log_locked(pool, worker_id, "job_completed", job.job_id);
        if (pool->queued == 0 && pool->active == 0)
            pthread_cond_broadcast(&pool->idle);
        pthread_mutex_unlock(&pool->mutex);
    }
}

void worker_pool_config_default(worker_pool_config_t *config)
{
    if (config == NULL)
        return;
    config->worker_count = 4;
    config->queue_capacity = 16;
    config->log_path = NULL;
}

const char *worker_pool_status_name(worker_pool_status_t status)
{
    switch (status) {
    case WORKER_POOL_SUCCESS: return "WORKER_POOL_SUCCESS";
    case WORKER_POOL_INVALID_ARGUMENT: return "WORKER_POOL_INVALID_ARGUMENT";
    case WORKER_POOL_ALREADY_INITIALIZED: return "WORKER_POOL_ALREADY_INITIALIZED";
    case WORKER_POOL_NOT_INITIALIZED: return "WORKER_POOL_NOT_INITIALIZED";
    case WORKER_POOL_QUEUE_FULL: return "WORKER_POOL_QUEUE_FULL";
    case WORKER_POOL_SHUTDOWN_IN_PROGRESS: return "WORKER_POOL_SHUTDOWN_IN_PROGRESS";
    case WORKER_POOL_THREAD_CREATE_FAILED: return "WORKER_POOL_THREAD_CREATE_FAILED";
    case WORKER_POOL_INTERNAL_ERROR: return "WORKER_POOL_INTERNAL_ERROR";
    default: return "WORKER_POOL_UNKNOWN";
    }
}

worker_pool_t *worker_pool_create(void)
{
    return calloc(1, sizeof(worker_pool_t));
}

worker_pool_status_t worker_pool_init(worker_pool_t *pool,
                                      const worker_pool_config_t *config)
{
    worker_pool_config_t defaults;
    size_t created = 0;

    if (pool == NULL || config == NULL)
        return WORKER_POOL_INVALID_ARGUMENT;
    if (pool->initialized)
        return WORKER_POOL_ALREADY_INITIALIZED;
    if (config->worker_count == 0 || config->worker_count > WORKER_POOL_MAX_WORKERS ||
        config->queue_capacity == 0 || config->queue_capacity > WORKER_POOL_MAX_QUEUE_CAPACITY)
        return WORKER_POOL_INVALID_ARGUMENT;
    defaults = *config;
    memset(pool, 0, sizeof(*pool));
    pool->worker_count = defaults.worker_count;
    pool->queue_capacity = defaults.queue_capacity;
    pool->threads = calloc(pool->worker_count, sizeof(*pool->threads));
    pool->stats = calloc(pool->worker_count, sizeof(*pool->stats));
    pool->jobs = calloc(pool->queue_capacity, sizeof(*pool->jobs));
    if (pool->threads == NULL || pool->stats == NULL || pool->jobs == NULL) {
        free(pool->threads);
        free(pool->stats);
        free(pool->jobs);
        memset(pool, 0, sizeof(*pool));
        return WORKER_POOL_INTERNAL_ERROR;
    }
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        free(pool->threads);
        free(pool->stats);
        free(pool->jobs);
        memset(pool, 0, sizeof(*pool));
        return WORKER_POOL_INTERNAL_ERROR;
    }
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&pool->mutex);
        free(pool->threads);
        free(pool->stats);
        free(pool->jobs);
        memset(pool, 0, sizeof(*pool));
        return WORKER_POOL_INTERNAL_ERROR;
    }
    if (pthread_cond_init(&pool->idle, NULL) != 0) {
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->mutex);
        free(pool->threads);
        free(pool->stats);
        free(pool->jobs);
        memset(pool, 0, sizeof(*pool));
        return WORKER_POOL_INTERNAL_ERROR;
    }
    if (config->log_path != NULL) {
        pool->log_file = fopen(config->log_path, "a");
        if (pool->log_file == NULL) {
            pthread_cond_destroy(&pool->idle);
            pthread_cond_destroy(&pool->not_empty);
            pthread_mutex_destroy(&pool->mutex);
            free(pool->threads);
            free(pool->stats);
            free(pool->jobs);
            memset(pool, 0, sizeof(*pool));
            return WORKER_POOL_INTERNAL_ERROR;
        }
    }
    pool->accepting = true;
    pool->initialized = true;
    for (size_t index = 0; index < pool->worker_count; index++) {
        worker_argument_t *argument = malloc(sizeof(*argument));

        if (argument == NULL)
            break;
        argument->pool = pool;
        argument->worker_id = (unsigned)index;
        pool->stats[index].worker_id = (unsigned)index;
        if (pthread_create(&pool->threads[index], NULL, worker_main, argument) != 0) {
            free(argument);
            break;
        }
        created++;
        pthread_mutex_lock(&pool->mutex);
        log_locked(pool, (unsigned)index, "worker_created", 0);
        pthread_mutex_unlock(&pool->mutex);
    }
    if (created != pool->worker_count) {
        pthread_mutex_lock(&pool->mutex);
        pool->accepting = false;
        pool->stopping = true;
        pthread_cond_broadcast(&pool->not_empty);
        pthread_mutex_unlock(&pool->mutex);
        for (size_t index = 0; index < created; index++)
            pthread_join(pool->threads[index], NULL);
        destroy_resources(pool);
        return WORKER_POOL_THREAD_CREATE_FAILED;
    }
    return WORKER_POOL_SUCCESS;
}

worker_pool_status_t worker_pool_submit(worker_pool_t *pool,
                                        const worker_job_t *job)
{
    if (pool == NULL || job == NULL || job->function == NULL)
        return WORKER_POOL_INVALID_ARGUMENT;
    if (!pool->initialized)
        return WORKER_POOL_NOT_INITIALIZED;
    pthread_mutex_lock(&pool->mutex);
    if (!pool->accepting || pool->stopping) {
        pthread_mutex_unlock(&pool->mutex);
        return WORKER_POOL_SHUTDOWN_IN_PROGRESS;
    }
    if (pool->queued >= pool->queue_capacity) {
        log_locked(pool, 0, "queue_full", job->job_id);
        pthread_mutex_unlock(&pool->mutex);
        return WORKER_POOL_QUEUE_FULL;
    }
    pool->jobs[pool->tail] = *job;
    pool->tail = (pool->tail + 1) % pool->queue_capacity;
    pool->queued++;
    log_locked(pool, 0, "job_submitted", job->job_id);
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    return WORKER_POOL_SUCCESS;
}

worker_pool_status_t worker_pool_wait_idle(worker_pool_t *pool)
{
    if (pool == NULL)
        return WORKER_POOL_INVALID_ARGUMENT;
    if (!pool->initialized)
        return WORKER_POOL_NOT_INITIALIZED;
    pthread_mutex_lock(&pool->mutex);
    while (pool->queued != 0 || pool->active != 0)
        pthread_cond_wait(&pool->idle, &pool->mutex);
    pthread_mutex_unlock(&pool->mutex);
    return WORKER_POOL_SUCCESS;
}

worker_pool_status_t worker_pool_shutdown(worker_pool_t *pool)
{
    if (pool == NULL)
        return WORKER_POOL_INVALID_ARGUMENT;
    if (!pool->initialized)
        return WORKER_POOL_NOT_INITIALIZED;
    pthread_mutex_lock(&pool->mutex);
    pool->accepting = false;
    pool->stopping = true;
    log_locked(pool, 0, "shutdown_started", 0);
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t index = 0; index < pool->worker_count; index++)
        pthread_join(pool->threads[index], NULL);
    destroy_resources(pool);
    return WORKER_POOL_SUCCESS;
}

worker_pool_status_t worker_pool_destroy(worker_pool_t *pool)
{
    worker_pool_status_t status;

    if (pool == NULL)
        return WORKER_POOL_INVALID_ARGUMENT;
    if (pool->initialized) {
        status = worker_pool_shutdown(pool);
        if (status != WORKER_POOL_SUCCESS)
            return status;
    }
    free(pool);
    return WORKER_POOL_SUCCESS;
}

size_t worker_pool_queued(const worker_pool_t *pool)
{
    size_t value;

    if (pool == NULL || !pool->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&pool->mutex);
    value = pool->queued;
    pthread_mutex_unlock((pthread_mutex_t *)&pool->mutex);
    return value;
}

size_t worker_pool_active(const worker_pool_t *pool)
{
    size_t value;

    if (pool == NULL || !pool->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&pool->mutex);
    value = pool->active;
    pthread_mutex_unlock((pthread_mutex_t *)&pool->mutex);
    return value;
}

size_t worker_pool_worker_count(const worker_pool_t *pool)
{
    return pool == NULL || !pool->initialized ? 0 : pool->worker_count;
}

size_t worker_pool_queue_capacity(const worker_pool_t *pool)
{
    return pool == NULL || !pool->initialized ? 0 : pool->queue_capacity;
}

size_t worker_pool_snapshot(const worker_pool_t *pool, worker_stats_t *stats,
                            size_t capacity)
{
    size_t count;

    if (pool == NULL || stats == NULL || capacity == 0 || !pool->initialized)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&pool->mutex);
    count = pool->worker_count < capacity ? pool->worker_count : capacity;
    memcpy(stats, pool->stats, count * sizeof(*stats));
    pthread_mutex_unlock((pthread_mutex_t *)&pool->mutex);
    return count;
}
