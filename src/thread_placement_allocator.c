#include "thread_placement_allocator.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { bool used, committed; pid_t pid, tid; uint64_t process_start, thread_start; int node, cpu; char attempt[128]; } entry_t;
struct thread_placement_allocator { entry_t entries[128]; };

static bool allocator_debug_enabled(void)
{
    const char *value = getenv("AWAVMA_ALLOCATOR_DEBUG");
    return value != NULL && strcmp(value, "1") == 0;
}

thread_placement_allocator_t *thread_placement_allocator_create(void) { return calloc(1, sizeof(thread_placement_allocator_t)); }
void thread_placement_allocator_destroy(thread_placement_allocator_t *allocator) { free(allocator); }

bool thread_placement_allocator_select(thread_placement_allocator_t *a, pid_t pid, uint64_t ps, pid_t tid,
                                       uint64_t ts, int node, const cpu_set_t *eligible, const char *attempt,
                                       bool reserve, int *cpu)
{
    unsigned counts[CPU_SETSIZE] = {0}; int selected = -1; entry_t *slot = NULL;
    bool debug = allocator_debug_enabled();
    if (a == NULL || eligible == NULL || attempt == NULL || cpu == NULL || CPU_COUNT(eligible) == 0) return false;
    if (debug) {
        bool first_cpu = true;
        fprintf(stderr, "AWAVMA_ALLOCATOR event=SELECT allocator_pointer=%p attempt=%s pid=%ld process_start=%llu tid=%ld thread_start=%llu node=%d reserve=%s eligible_count=%d eligible_cpus=",
                (void *)a, attempt, (long)pid, (unsigned long long)ps, (long)tid,
                (unsigned long long)ts, node, reserve ? "true" : "false", CPU_COUNT(eligible));
        for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, eligible)) {
            fprintf(stderr, "%s%d", first_cpu ? "" : ",", c);
            first_cpu = false;
        }
        fputc('\n', stderr);
    }
    for (unsigned i = 0; i < 128; i++) {
        entry_t *e = &a->entries[i];
        if (!e->used) { if (!slot) slot = e; continue; }
        if (debug && e->pid == pid && e->process_start == ps && e->node == node)
            fprintf(stderr, "AWAVMA_ALLOCATOR entry slot=%u pid=%ld process_start=%llu tid=%ld thread_start=%llu node=%d cpu=%d committed=%s attempt=%s\n",
                    i, (long)e->pid, (unsigned long long)e->process_start, (long)e->tid,
                    (unsigned long long)e->thread_start, e->node, e->cpu,
                    e->committed ? "true" : "false", e->attempt[0] ? e->attempt : "NONE");
        if (e->pid == pid && e->process_start == ps && e->tid == tid && e->thread_start == ts && e->node == node && e->committed) {
            *cpu = e->cpu;
            if (debug) fprintf(stderr, "AWAVMA_ALLOCATOR selected_cpu=%d reservation_slot=NONE\n", *cpu);
            return true;
        }
        if (e->pid == pid && e->process_start == ps && e->node == node && CPU_ISSET(e->cpu, eligible)) counts[e->cpu]++;
        if (!slot && !e->used) slot = e;
    }
    for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, eligible) && (selected < 0 || counts[c] < counts[selected])) selected = c;
    if (debug) {
        for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, eligible) && counts[c] > 0)
            fprintf(stderr, "AWAVMA_ALLOCATOR count cpu=%d active=%u\n", c, counts[c]);
        fprintf(stderr, "AWAVMA_ALLOCATOR selected_cpu=%d reservation_slot=%ld\n", selected,
                reserve && slot != NULL ? (long)(slot - a->entries) : -1L);
    }
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
    bool debug = allocator_debug_enabled();
    if (a == NULL || attempt == NULL) return;
    for (unsigned i = 0; i < 128; i++) if (a->entries[i].used && strcmp(a->entries[i].attempt, attempt) == 0) {
        if (debug) fprintf(stderr, "AWAVMA_ALLOCATOR event=FINISH allocator_pointer=%p finish_attempt=%s committed_argument=%s matching_slot=%u before pid=%ld process_start=%llu tid=%ld thread_start=%llu node=%d cpu=%d committed=%s attempt=%s\n",
                           (void *)a, attempt, committed ? "true" : "false", i, (long)a->entries[i].pid,
                           (unsigned long long)a->entries[i].process_start, (long)a->entries[i].tid,
                           (unsigned long long)a->entries[i].thread_start, a->entries[i].node,
                           a->entries[i].cpu, a->entries[i].committed ? "true" : "false", a->entries[i].attempt);
        if (committed) { a->entries[i].committed = true; a->entries[i].attempt[0] = '\0'; }
        else memset(&a->entries[i], 0, sizeof(a->entries[i]));
        if (debug && a->entries[i].used) fprintf(stderr, "AWAVMA_ALLOCATOR event=FINISH after slot=%u pid=%ld process_start=%llu tid=%ld thread_start=%llu node=%d cpu=%d committed=%s attempt=%s\n",
                                                    i, (long)a->entries[i].pid,
                                                    (unsigned long long)a->entries[i].process_start,
                                                    (long)a->entries[i].tid,
                                                    (unsigned long long)a->entries[i].thread_start,
                                                    a->entries[i].node, a->entries[i].cpu,
                                                    a->entries[i].committed ? "true" : "false",
                                                    a->entries[i].attempt[0] ? a->entries[i].attempt : "NONE");
        return;
    }
    if (debug) fprintf(stderr, "AWAVMA_ALLOCATOR event=FINISH allocator_pointer=%p finish_attempt=%s committed_argument=%s matching_slot=NONE\n",
                       (void *)a, attempt, committed ? "true" : "false");
}
