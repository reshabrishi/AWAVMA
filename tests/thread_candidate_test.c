#define _POSIX_C_SOURCE 200809L
#include "runtime_migration_metadata.h"
#include "thread_candidate.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static atomic_bool ready;
static void *worker(void *unused) { (void)unused; atomic_store(&ready, true); sleep(2); return NULL; }

int main(void)
{
    pthread_t thread;
    thread_candidate_t first, second;
    uint64_t ticks;
    assert(runtime_read_start_time_ticks(getpid(), &ticks));
    assert(pthread_create(&thread, NULL, worker, NULL) == 0);
    while (!atomic_load(&ready)) usleep(1000);
    assert(thread_candidate_select(getpid(), ticks, &first));
    assert(first.verified && first.tid > 0 && first.start_time_ticks_available);
    assert(thread_candidate_select(getpid(), ticks, &second));
    assert(first.tid == second.tid);
    assert(!thread_candidate_select(getpid(), ticks + 1, &second));
    assert(pthread_join(thread, NULL) == 0);
    printf("thread_candidate_test: PASS\n");
    return 0;
}
