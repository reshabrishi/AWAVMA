#ifndef AWAVMA_THREAD_PLACEMENT_ALLOCATOR_H
#define AWAVMA_THREAD_PLACEMENT_ALLOCATOR_H

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct thread_placement_allocator thread_placement_allocator_t;

thread_placement_allocator_t *thread_placement_allocator_create(void);
void thread_placement_allocator_destroy(thread_placement_allocator_t *allocator);
bool thread_placement_allocator_select(thread_placement_allocator_t *allocator, pid_t pid,
                                       uint64_t process_start, pid_t tid, uint64_t thread_start,
                                       int node, const cpu_set_t *eligible, const char *attempt_id,
                                       bool reserve, int *cpu);
void thread_placement_allocator_finish(thread_placement_allocator_t *allocator,
                                       const char *attempt_id, bool committed);
#endif
