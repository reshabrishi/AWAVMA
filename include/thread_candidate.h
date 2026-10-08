#ifndef AWAVMA_THREAD_CANDIDATE_H
#define AWAVMA_THREAD_CANDIDATE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    pid_t tid;
    uint64_t start_time_ticks;
    bool start_time_ticks_available;
    int current_cpu;
    bool current_cpu_available;
    bool verified;
    char provenance[64];
} thread_candidate_t;

/* Selects the lowest verified non-leader TID, then the verified leader if it is the only task. */
bool thread_candidate_select(pid_t pid, uint64_t process_start_time_ticks,
                             thread_candidate_t *candidate);

#endif
