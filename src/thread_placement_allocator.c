#include "thread_placement_allocator.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { bool used, committed; pid_t pid, tid; uint64_t process_start, thread_start; int node, cpu; char attempt[128]; } entry_t;
struct thread_placement_allocator { entry_t entries[128]; };

thread_placement_allocator_t *thread_placement_allocator_create(void) { return calloc(1, sizeof(thread_placement_allocator_t)); }
void thread_placement_allocator_destroy(thread_placement_allocator_t *allocator) { free(allocator); }

bool thread_placement_allocator_select(thread_placement_allocator_t *a, pid_t pid, uint64_t ps, pid_t tid,
                                       uint64_t ts, int node, const cpu_set_t *eligible, const char *attempt,
                                       bool reserve, int *cpu)
{
    unsigned counts[CPU_SETSIZE] = {0}; int selected = -1; entry_t *slot = NULL;
    if (a == NULL || eligible == NULL || attempt == NULL || cpu == NULL || CPU_COUNT(eligible) == 0) return false;
    for (unsigned i = 0; i < 128; i++) {
        entry_t *e = &a->entries[i];
        if (!e->used) { if (!slot) slot = e; continue; }
        if (e->pid == pid && e->process_start == ps && e->tid == tid && e->thread_start == ts && e->node == node && e->committed) { *cpu = e->cpu; return true; }
        if (e->pid == pid && e->process_start == ps && e->node == node && CPU_ISSET(e->cpu, eligible)) counts[e->cpu]++;
        if (!slot && !e->used) slot = e;
    }
    for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, eligible) && (selected < 0 || counts[c] < counts[selected])) selected = c;
    if (selected < 0) return false;
    *cpu = selected;
    if (!reserve) return true;
    if (!slot) return false;
    *slot = (entry_t){.used=true,.pid=pid,.tid=tid,.process_start=ps,.thread_start=ts,.node=node,.cpu=selected};
    snprintf(slot->attempt, sizeof(slot->attempt), "%s", attempt);
    return true;
}

void thread_placement_allocator_finish(thread_placement_allocator_t *a, const char *attempt, bool committed)
{
    if (a == NULL || attempt == NULL) return;
    for (unsigned i = 0; i < 128; i++) if (a->entries[i].used && strcmp(a->entries[i].attempt, attempt) == 0) {
        if (committed) { a->entries[i].committed = true; a->entries[i].attempt[0] = '\0'; }
        else memset(&a->entries[i], 0, sizeof(a->entries[i]));
        return;
    }
}
