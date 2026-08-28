#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    volatile uint64_t *data;
    size_t elements;
    size_t thread_id;
    size_t thread_count;
    double end_time;
    uint64_t checksum;
} target_worker_t;

static double now_seconds(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void *target_worker(void *argument)
{
    target_worker_t *worker = argument;
    uint64_t checksum = 0;

    while (now_seconds() < worker->end_time) {
        size_t index;

        for (index = worker->thread_id; index < worker->elements; index += worker->thread_count)
            checksum ^= worker->data[index] + index;
    }
    worker->checksum = checksum;
    return NULL;
}

static int parse_positive(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0)
        return -1;
    *value = parsed;
    return 0;
}

int main(int argc, char **argv)
{
    size_t threads = 2;
    size_t memory_mb = 16;
    double duration = 3.0;
    size_t elements;
    volatile uint64_t *data;
    pthread_t *worker_threads;
    target_worker_t *workers;
    uint64_t parsed;
    int index;
    size_t created = 0;
    uint64_t checksum = 0;
    double end_time;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--threads") == 0 && index + 1 < argc) {
            if (parse_positive(argv[++index], &parsed) != 0 || parsed > SIZE_MAX)
                return EXIT_FAILURE;
            threads = (size_t)parsed;
        } else if (strcmp(argv[index], "--memory-mb") == 0 && index + 1 < argc) {
            if (parse_positive(argv[++index], &parsed) != 0 || parsed > SIZE_MAX)
                return EXIT_FAILURE;
            memory_mb = (size_t)parsed;
        } else if (strcmp(argv[index], "--duration") == 0 && index + 1 < argc) {
            char *end = NULL;
            duration = strtod(argv[++index], &end);
            if (end == argv[index] || *end != '\0' || duration <= 0.0)
                return EXIT_FAILURE;
        } else {
            fprintf(stderr, "Usage: %s [--threads N] [--memory-mb N] [--duration SEC]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (memory_mb > SIZE_MAX / (1024U * 1024U) ||
        memory_mb * 1024U * 1024U / sizeof(uint64_t) < threads)
        return EXIT_FAILURE;
    elements = memory_mb * 1024U * 1024U / sizeof(uint64_t);
    data = NULL;
    if (posix_memalign((void **)&data, 64, memory_mb * 1024U * 1024U) != 0)
        return EXIT_FAILURE;
    for (size_t element = 0; element < elements; element++)
        data[element] = element;
    worker_threads = calloc(threads, sizeof(*worker_threads));
    workers = calloc(threads, sizeof(*workers));
    if (worker_threads == NULL || workers == NULL)
        return EXIT_FAILURE;
    end_time = now_seconds() + duration;
    for (size_t worker = 0; worker < threads; worker++) {
        workers[worker].data = data;
        workers[worker].elements = elements;
        workers[worker].thread_id = worker;
        workers[worker].thread_count = threads;
        workers[worker].end_time = end_time;
        if (pthread_create(&worker_threads[worker], NULL, target_worker, &workers[worker]) != 0)
            break;
        created++;
    }
    for (size_t worker = 0; worker < created; worker++)
        pthread_join(worker_threads[worker], NULL);
    if (created != threads) {
        free(workers);
        free(worker_threads);
        free((void *)data);
        return EXIT_FAILURE;
    }
    for (size_t worker = 0; worker < threads; worker++)
        checksum ^= workers[worker].checksum;
    printf("monitor test target completed: checksum=%llu\n", (unsigned long long)checksum);
    free(workers);
    free(worker_threads);
    free((void *)data);
    return EXIT_SUCCESS;
}
