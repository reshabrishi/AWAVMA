#define _POSIX_C_SOURCE 200809L

#include "worker_pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static FILE *result_file;
static int failures;

static void report_result(const char *id, const char *expected, const char *actual,
                          int passed, const char *reason)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
    if (result_file != NULL)
        fprintf(result_file, "%s,%s,%s,%s,%s\n", id, expected, actual,
                passed ? "PASS" : "FAIL", reason == NULL ? "-" : reason);
    if (!passed)
        failures++;
}

static void short_sleep(unsigned milliseconds)
{
    struct timespec delay = {milliseconds / 1000U,
                             (long)(milliseconds % 1000U) * 1000000L};

    nanosleep(&delay, NULL);
}

static worker_pool_t *new_pool(size_t workers, size_t capacity)
{
    worker_pool_config_t config;
    worker_pool_t *pool;

    worker_pool_config_default(&config);
    config.worker_count = workers;
    config.queue_capacity = capacity;
    config.log_path = "logs/worker_pool.log";
    pool = worker_pool_create();
    if (pool == NULL || worker_pool_init(pool, &config) != WORKER_POOL_SUCCESS) {
        worker_pool_destroy(pool);
        return NULL;
    }
    return pool;
}

static int finish_pool(worker_pool_t *pool)
{
    worker_pool_status_t wait_status;
    worker_pool_status_t shutdown_status;

    if (pool == NULL)
        return 0;
    wait_status = worker_pool_wait_idle(pool);
    shutdown_status = worker_pool_shutdown(pool);
    worker_pool_destroy(pool);
    return wait_status == WORKER_POOL_SUCCESS && shutdown_status == WORKER_POOL_SUCCESS;
}

typedef struct {
    _Atomic int count;
    unsigned sleep_ms;
} count_context_t;

static void count_job(void *argument)
{
    count_context_t *context = argument;

    atomic_fetch_add(&context->count, 1);
    if (context->sleep_ms > 0)
        short_sleep(context->sleep_ms);
}

typedef struct {
    _Atomic int active;
    _Atomic int maximum;
} overlap_context_t;

static void overlap_job(void *argument)
{
    overlap_context_t *context = argument;
    int active = atomic_fetch_add(&context->active, 1) + 1;
    int maximum = atomic_load(&context->maximum);

    while (active > maximum &&
           !atomic_compare_exchange_weak(&context->maximum, &maximum, active)) {}
    short_sleep(25);
    atomic_fetch_sub(&context->active, 1);
}

typedef struct {
    int value;
    int *order;
    _Atomic int *next;
} fifo_argument_t;

static void fifo_job(void *argument)
{
    fifo_argument_t *context = argument;

    context->order[atomic_fetch_add(context->next, 1)] = context->value;
}

typedef struct {
    int *source;
    int *received;
} argument_context_t;

static void argument_job(void *argument)
{
    argument_context_t *context = argument;

    *context->received = *context->source;
}

static void fault_job(void *argument)
{
    atomic_fetch_add((_Atomic int *)argument, 1);
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int started;
    int release;
} blocking_context_t;

static void blocking_init(blocking_context_t *context)
{
    memset(context, 0, sizeof(*context));
    pthread_mutex_init(&context->mutex, NULL);
    pthread_cond_init(&context->condition, NULL);
}

static void blocking_destroy(blocking_context_t *context)
{
    pthread_cond_destroy(&context->condition);
    pthread_mutex_destroy(&context->mutex);
}

static void blocking_job(void *argument)
{
    blocking_context_t *context = argument;

    pthread_mutex_lock(&context->mutex);
    context->started = 1;
    pthread_cond_broadcast(&context->condition);
    while (!context->release)
        pthread_cond_wait(&context->condition, &context->mutex);
    pthread_mutex_unlock(&context->mutex);
}

static void blocking_wait_started(blocking_context_t *context)
{
    pthread_mutex_lock(&context->mutex);
    while (!context->started)
        pthread_cond_wait(&context->condition, &context->mutex);
    pthread_mutex_unlock(&context->mutex);
}

static void blocking_release(blocking_context_t *context)
{
    pthread_mutex_lock(&context->mutex);
    context->release = 1;
    pthread_cond_broadcast(&context->condition);
    pthread_mutex_unlock(&context->mutex);
}

typedef struct {
    _Atomic int seen[100];
    _Atomic int total;
    int index;
} exactly_once_argument_t;

static void exactly_once_job(void *argument)
{
    exactly_once_argument_t *context = argument;

    atomic_fetch_add(&context->seen[context->index], 1);
    atomic_fetch_add(&context->total, 1);
}

typedef struct {
    worker_pool_t *pool;
    count_context_t *job_context;
    int base;
    int count;
    _Atomic int *accepted;
    _Atomic int *failed;
} producer_context_t;

static void *producer_thread(void *argument)
{
    producer_context_t *context = argument;

    for (int index = 0; index < context->count; index++) {
        worker_job_t job = {count_job, context->job_context,
                            (uint64_t)(context->base + index)};
        worker_pool_status_t status = worker_pool_submit(context->pool, &job);

        if (status == WORKER_POOL_SUCCESS)
            atomic_fetch_add(context->accepted, 1);
        else
            atomic_fetch_add(context->failed, 1);
    }
    return NULL;
}

typedef struct {
    worker_pool_t *pool;
    worker_pool_status_t status;
} shutdown_context_t;

static void *shutdown_thread(void *argument)
{
    shutdown_context_t *context = argument;

    context->status = worker_pool_shutdown(context->pool);
    return NULL;
}

int main(void)
{
    worker_pool_t *pool;
    worker_pool_config_t config;
    worker_stats_t stats[8];
    worker_job_t job;
    count_context_t count_context;
    overlap_context_t overlap;
    blocking_context_t blocking;
    exactly_once_argument_t once_arguments[100];
    int passed;

    mkdir("logs", 0755);
    mkdir("results", 0755);
    result_file = fopen("results/worker_pool_results.csv", "w");
    if (result_file != NULL)
        fputs("test_id,expected,actual,result,reason\n", result_file);

    pool = new_pool(2, 4);
    report_result("W01", "initialized", pool == NULL ? "failed" : "initialized", pool != NULL, "pool initialization");
    finish_pool(pool);

    worker_pool_config_default(&config);
    config.worker_count = 0;
    pool = worker_pool_create();
    passed = pool != NULL && worker_pool_init(pool, &config) == WORKER_POOL_INVALID_ARGUMENT;
    report_result("W02", "invalid argument", passed ? "rejected" : "accepted", passed, "zero workers");
    worker_pool_destroy(pool);

    worker_pool_config_default(&config);
    config.queue_capacity = 0;
    pool = worker_pool_create();
    passed = pool != NULL && worker_pool_init(pool, &config) == WORKER_POOL_INVALID_ARGUMENT;
    report_result("W03", "invalid argument", passed ? "rejected" : "accepted", passed, "zero queue capacity");
    worker_pool_destroy(pool);

    pool = new_pool(4, 4);
    passed = pool != NULL && worker_pool_worker_count(pool) == 4 &&
             worker_pool_snapshot(pool, stats, 8) == 4;
    for (size_t index = 0; passed && index < 4; index++)
        passed = stats[index].worker_id == index;
    report_result("W04", "four workers", passed ? "four workers" : "wrong count", passed, "worker IDs");
    finish_pool(pool);

    memset(&count_context, 0, sizeof(count_context));
    pool = new_pool(2, 4);
    job = (worker_job_t){count_job, &count_context, 1};
    passed = pool != NULL && worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS &&
             worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS && atomic_load(&count_context.count) == 1;
    report_result("W05", "one job", passed ? "executed" : "missing", passed, "single callback");
    finish_pool(pool);

    memset(&count_context, 0, sizeof(count_context));
    pool = new_pool(4, 16);
    passed = pool != NULL;
    for (int index = 0; passed && index < 12; index++) {
        job = (worker_job_t){count_job, &count_context, (uint64_t)(index + 1)};
        passed = worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS;
    }
    passed = passed && worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS &&
             atomic_load(&count_context.count) == 12;
    report_result("W06", "twelve jobs", passed ? "twelve executed" : "wrong count", passed, "multiple callbacks");
    finish_pool(pool);

    memset(&overlap, 0, sizeof(overlap));
    pool = new_pool(4, 8);
    for (int index = 0; pool != NULL && index < 8; index++) {
        job = (worker_job_t){overlap_job, &overlap, (uint64_t)(index + 1)};
        if (worker_pool_submit(pool, &job) != WORKER_POOL_SUCCESS)
            passed = 0;
    }
    passed = pool != NULL && worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS &&
             atomic_load(&overlap.maximum) > 1;
    report_result("W07", "overlap with four workers", passed ? "overlap" : "serialized", passed, "concurrency");
    finish_pool(pool);

    {
        int order[5] = {0};
        _Atomic int next = 0;
        worker_pool_t *fifo_pool = new_pool(1, 5);
        fifo_argument_t values[5];

        passed = fifo_pool != NULL;
        for (int index = 0; passed && index < 5; index++) {
            values[index] = (fifo_argument_t){index + 1, order, &next};
            worker_job_t fifo = {fifo_job, &values[index], (uint64_t)(index + 1)};
            passed = worker_pool_submit(fifo_pool, &fifo) == WORKER_POOL_SUCCESS;
        }
        passed = passed && worker_pool_wait_idle(fifo_pool) == WORKER_POOL_SUCCESS;
        for (int index = 0; passed && index < 5; index++)
            passed = order[index] == index + 1;
        report_result("W08", "FIFO order", passed ? "FIFO" : "reordered", passed, "single worker queue");
        finish_pool(fifo_pool);
    }

    blocking_init(&blocking);
    pool = new_pool(1, 3);
    job = (worker_job_t){blocking_job, &blocking, 1};
    passed = pool != NULL && worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS;
    blocking_wait_started(&blocking);
    for (int index = 0; passed && index < 2; index++) {
        job = (worker_job_t){count_job, &count_context, (uint64_t)(index + 2)};
        passed = worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS;
    }
    passed = passed && worker_pool_queued(pool) == 2;
    report_result("W09", "queue size two", passed ? "two" : "wrong size", passed, "queue accounting");
    blocking_release(&blocking);
    finish_pool(pool);
    blocking_destroy(&blocking);

    blocking_init(&blocking);
    pool = new_pool(1, 2);
    job = (worker_job_t){blocking_job, &blocking, 1};
    passed = pool != NULL && worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS;
    blocking_wait_started(&blocking);
    for (int index = 0; passed && index < 2; index++) {
        job = (worker_job_t){count_job, &count_context, (uint64_t)(index + 2)};
        passed = worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS;
    }
    job = (worker_job_t){count_job, &count_context, 4};
    passed = passed && worker_pool_submit(pool, &job) == WORKER_POOL_QUEUE_FULL;
    report_result("W10", "queue full status", passed ? "WORKER_POOL_QUEUE_FULL" : "accepted", passed, "nonblocking enqueue");
    blocking_release(&blocking);
    finish_pool(pool);
    blocking_destroy(&blocking);

    pool = new_pool(2, 2);
    short_sleep(20);
    passed = pool != NULL && worker_pool_queued(pool) == 0 && worker_pool_active(pool) == 0;
    report_result("W11", "workers wait", passed ? "idle" : "busy", passed, "condition variable wait");
    job = (worker_job_t){count_job, &count_context, 1};
    passed = passed && worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS &&
             worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS;
    report_result("W12", "worker wakes", passed ? "woke" : "not executed", passed, "job notification");
    finish_pool(pool);

    {
        int expected = 42;
        int received = 0;
        argument_context_t argument_context = {&expected, &received};

        pool = new_pool(1, 1);
        job = (worker_job_t){argument_job, &argument_context, 42};
        passed = pool != NULL && worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS &&
                 worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS && received == expected;
        report_result("W13", "callback argument", passed ? "correct" : "wrong", passed, "generic argument");
        finish_pool(pool);
    }

    {
        memset(once_arguments, 0, sizeof(once_arguments));
        pool = new_pool(4, 128);
        passed = pool != NULL;
        for (int index = 0; passed && index < 100; index++) {
            once_arguments[index].index = index;
            job = (worker_job_t){exactly_once_job, &once_arguments[index], (uint64_t)(index + 1)};
            passed = worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS;
        }
        passed = passed && worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS;
        for (int index = 0; passed && index < 100; index++)
            passed = atomic_load(&once_arguments[index].seen[index]) == 1;
        if (passed) {
            int total = 0;
            for (int index = 0; index < 100; index++)
                total += atomic_load(&once_arguments[index].seen[index]);
            passed = total == 100;
        }
        report_result("W14", "100 exactly once", passed ? "100" : "wrong count", passed, "exactly-once callbacks");
        finish_pool(pool);
    }

    {
        pthread_t producers[4];
        producer_context_t producer_context[4];
        _Atomic int accepted = 0;
        _Atomic int failed = 0;

        memset(&count_context, 0, sizeof(count_context));
        pool = new_pool(4, 256);
        passed = pool != NULL;
        for (int index = 0; passed && index < 4; index++) {
            producer_context[index] = (producer_context_t){pool, &count_context, index * 25, 25,
                                                           &accepted, &failed};
            passed = pthread_create(&producers[index], NULL, producer_thread,
                                    &producer_context[index]) == 0;
        }
        for (int index = 0; index < 4 && pool != NULL; index++)
            pthread_join(producers[index], NULL);
        passed = passed && worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS &&
                 atomic_load(&failed) == 0 && atomic_load(&accepted) == 100 &&
                 atomic_load(&count_context.count) == 100;
        report_result("W15", "100 producer submissions", passed ? "100" : "wrong count", passed, "concurrent producers");
        finish_pool(pool);
    }

    memset(&count_context, 0, sizeof(count_context));
    pool = new_pool(4, 32);
    for (int index = 0; pool != NULL && index < 20; index++) {
        job = (worker_job_t){count_job, &count_context, (uint64_t)(index + 1)};
        if (worker_pool_submit(pool, &job) != WORKER_POOL_SUCCESS)
            passed = 0;
    }
    passed = pool != NULL && worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS &&
             worker_pool_snapshot(pool, stats, 8) == 4;
    {
        uint64_t completed = 0;
        for (size_t index = 0; passed && index < 4; index++) {
            passed = !stats[index].busy;
            completed += stats[index].jobs_completed;
        }
        passed = passed && completed == 20;
    }
    report_result("W16", "20 completed in stats", passed ? "consistent" : "inconsistent", passed, "worker statistics");
    finish_pool(pool);

    pool = new_pool(2, 2);
    passed = pool != NULL && worker_pool_shutdown(pool) == WORKER_POOL_SUCCESS;
    report_result("W17", "empty graceful shutdown", passed ? "shutdown" : "failure", passed, "empty queue");
    worker_pool_destroy(pool);

    memset(&count_context, 0, sizeof(count_context));
    pool = new_pool(2, 10);
    for (int index = 0; pool != NULL && index < 10; index++) {
        job = (worker_job_t){count_job, &count_context, (uint64_t)(index + 1)};
        if (worker_pool_submit(pool, &job) != WORKER_POOL_SUCCESS)
            passed = 0;
    }
    passed = pool != NULL && worker_pool_shutdown(pool) == WORKER_POOL_SUCCESS &&
             atomic_load(&count_context.count) == 10;
    report_result("W18", "pending jobs finish", passed ? "10 finished" : "missing jobs", passed, "graceful policy");
    worker_pool_destroy(pool);

    blocking_init(&blocking);
    pool = new_pool(1, 1);
    job = (worker_job_t){blocking_job, &blocking, 1};
    passed = pool != NULL && worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS;
    blocking_wait_started(&blocking);
    shutdown_context_t shutdown_context = {pool, WORKER_POOL_INTERNAL_ERROR};
    pthread_t shutdown_thread_id;
    passed = passed && pthread_create(&shutdown_thread_id, NULL, shutdown_thread, &shutdown_context) == 0;
    short_sleep(10);
    job = (worker_job_t){count_job, &count_context, 2};
    worker_pool_status_t submit_status = worker_pool_submit(pool, &job);
    passed = passed && submit_status == WORKER_POOL_SHUTDOWN_IN_PROGRESS;
    blocking_release(&blocking);
    pthread_join(shutdown_thread_id, NULL);
    passed = passed && shutdown_context.status == WORKER_POOL_SUCCESS;
    report_result("W19", "reject after shutdown begins", passed ? worker_pool_status_name(submit_status) : "accepted", passed, "shutdown admission");
    worker_pool_destroy(pool);
    blocking_destroy(&blocking);

    pool = new_pool(3, 3);
    passed = pool != NULL && worker_pool_shutdown(pool) == WORKER_POOL_SUCCESS;
    report_result("W20", "all workers joined", passed ? "joined" : "failure", passed, "shutdown join");
    worker_pool_destroy(pool);

    pool = new_pool(2, 2);
    passed = pool != NULL && worker_pool_shutdown(pool) == WORKER_POOL_SUCCESS;
    job = (worker_job_t){count_job, &count_context, 1};
    passed = passed && worker_pool_submit(pool, &job) == WORKER_POOL_NOT_INITIALIZED;
    report_result("W21", "no worker after shutdown", passed ? "rejected" : "accepted", passed, "post-shutdown API");
    worker_pool_destroy(pool);

    passed = 1;
    for (int cycle = 0; cycle < 3; cycle++) {
        pool = new_pool(2, 2);
        passed = passed && pool != NULL && worker_pool_shutdown(pool) == WORKER_POOL_SUCCESS;
        worker_pool_destroy(pool);
    }
    report_result("W22", "three lifecycle cycles", passed ? "three" : "failure", passed, "reinitialize after shutdown");

    {
        _Atomic int fault_count = 0;
        memset(&count_context, 0, sizeof(count_context));
        pool = new_pool(2, 4);
        job = (worker_job_t){fault_job, &fault_count, 1};
        passed = pool != NULL && worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS;
        job = (worker_job_t){count_job, &count_context, 2};
        passed = passed && worker_pool_submit(pool, &job) == WORKER_POOL_SUCCESS &&
                 worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS &&
                 atomic_load(&fault_count) == 1 && atomic_load(&count_context.count) == 1;
        report_result("W23", "fault callback survives", passed ? "continued" : "stopped", passed, "callback returned normally");
        finish_pool(pool);
    }

    {
        pthread_t producers[8];
        producer_context_t producer_context[8];
        _Atomic int accepted = 0;
        _Atomic int failed = 0;

        memset(&count_context, 0, sizeof(count_context));
        pool = new_pool(4, 512);
        passed = pool != NULL;
        for (int index = 0; passed && index < 8; index++) {
            producer_context[index] = (producer_context_t){pool, &count_context, index * 50, 50,
                                                           &accepted, &failed};
            passed = pthread_create(&producers[index], NULL, producer_thread,
                                    &producer_context[index]) == 0;
        }
        for (int index = 0; index < 8 && pool != NULL; index++)
            pthread_join(producers[index], NULL);
        passed = passed && worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS &&
                 atomic_load(&failed) == 0 && atomic_load(&accepted) == 400 &&
                 atomic_load(&count_context.count) == 400;
        report_result("W24", "400 concurrent jobs", passed ? "400" : "corruption", passed, "stress submission");
        finish_pool(pool);
    }

    memset(&count_context, 0, sizeof(count_context));
    pool = new_pool(2, 32);
    for (int index = 0; pool != NULL && index < 20; index++) {
        job = (worker_job_t){count_job, &count_context, (uint64_t)(index + 1)};
        if (worker_pool_submit(pool, &job) != WORKER_POOL_SUCCESS)
            passed = 0;
    }
    passed = pool != NULL && worker_pool_wait_idle(pool) == WORKER_POOL_SUCCESS &&
             atomic_load(&count_context.count) == 20;
    report_result("W25", "no deadlock", passed ? "completed" : "blocked", passed, "idle wait and shutdown path");
    finish_pool(pool);

    if (result_file != NULL)
        fclose(result_file);
    printf("Worker pool tests: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
