#define _POSIX_C_SOURCE 200809L

#include "runtime_migration_metadata.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool read_stat(pid_t pid, uint64_t *start_ticks, uint64_t *cpu_ticks, int *cpu)
{
    char path[64];
    char line[4096];
    char *cursor;
    char *save = NULL;
    FILE *file;
    bool have_start = false;
    bool have_cpu_time = false;

    if (pid <= 0 || start_ticks == NULL || cpu_ticks == NULL || cpu == NULL ||
        snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path))
        return false;
    file = fopen(path, "r");
    if (file == NULL)
        return false;
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }
    fclose(file);
    cursor = strrchr(line, ')');
    if (cursor == NULL || cursor[1] != ' ')
        return false;
    cursor += 2;
    for (int field = 3; field <= 39; field++) {
        char *token = strtok_r(field == 3 ? cursor : NULL, " ", &save);
        char *end = NULL;
        unsigned long long value;

        if (token == NULL)
            return false;
        if (field != 14 && field != 15 && field != 22 && field != 39)
            continue;
        errno = 0;
        value = strtoull(token, &end, 10);
        if (errno != 0 || end == token || (*end != '\0' && *end != '\n'))
            return false;
        if (field == 14) {
            *cpu_ticks = (uint64_t)value;
            have_cpu_time = true;
        } else if (field == 15 && have_cpu_time) {
            *cpu_ticks += (uint64_t)value;
        } else if (field == 22) {
            *start_ticks = (uint64_t)value;
            have_start = true;
        } else if (field == 39 && value <= INT_MAX) {
            *cpu = (int)value;
        }
    }
    return have_start && have_cpu_time;
}

bool runtime_get_migration_metadata(pid_t pid, uint64_t start_time_ticks,
                                    RuntimeMigrationMetadata *metadata)
{
    uint64_t observed_start = 0;
    uint64_t cpu_ticks = 0;
    int cpu = -1;

    if (metadata == NULL)
        return false;
    memset(metadata, 0, sizeof(*metadata));
    metadata->pid = pid;
    metadata->start_time_ticks = start_time_ticks;
    metadata->captured_at_ms = monotonic_ms();
    metadata->current_cpu = -1;
    metadata->current_numa_node = -1;
    if (!read_stat(pid, &observed_start, &cpu_ticks, &cpu))
        return true;
    metadata->process_exists = true;
    metadata->identity_match = start_time_ticks != 0 && observed_start == start_time_ticks;
    if (!metadata->identity_match)
        return true;
    metadata->process_cpu_time_available = true;
    metadata->process_cpu_time_ticks = cpu_ticks;
    if (cpu >= 0) {
        metadata->current_cpu_available = true;
        metadata->current_cpu = cpu;
    }
    if (sched_getaffinity(pid, sizeof(metadata->affinity), &metadata->affinity) == 0)
        metadata->affinity_available = true;
    /* The runtime has no node-of-CPU provider without libnuma; do not infer node zero. */
    return true;
}

bool runtime_migration_checkpoint_affinity(const RuntimeMigrationMetadata *metadata,
                                           RuntimeMigrationCheckpoint *checkpoint)
{
    if (checkpoint == NULL)
        return false;
    memset(checkpoint, 0, sizeof(*checkpoint));
    if (metadata == NULL || !metadata->identity_match || !metadata->affinity_available)
        return false;
    checkpoint->captured_at_ms = monotonic_ms();
    checkpoint->pid = metadata->pid;
    checkpoint->start_time_ticks = metadata->start_time_ticks;
    checkpoint->affinity_available = true;
    checkpoint->original_affinity = metadata->affinity;
    return true;
}

RuntimeMigrationRollbackResult runtime_migration_restore_affinity(
    const RuntimeMigrationCheckpoint *checkpoint)
{
    RuntimeMigrationMetadata current;
    cpu_set_t restored;

    if (checkpoint == NULL || !checkpoint->affinity_available)
        return RUNTIME_MIGRATION_ROLLBACK_CHECKPOINT_UNAVAILABLE;
    if (!runtime_get_migration_metadata(checkpoint->pid, checkpoint->start_time_ticks, &current))
        return RUNTIME_MIGRATION_ROLLBACK_FAILED;
    if (!current.process_exists)
        return RUNTIME_MIGRATION_ROLLBACK_TARGET_GONE;
    if (!current.identity_match)
        return RUNTIME_MIGRATION_ROLLBACK_IDENTITY_CHANGED;
    if (sched_setaffinity(checkpoint->pid, sizeof(checkpoint->original_affinity),
                          &checkpoint->original_affinity) != 0)
        return RUNTIME_MIGRATION_ROLLBACK_FAILED;
    if (sched_getaffinity(checkpoint->pid, sizeof(restored), &restored) != 0)
        return RUNTIME_MIGRATION_ROLLBACK_VERIFICATION_FAILED;
    return CPU_EQUAL(&restored, &checkpoint->original_affinity) ?
        RUNTIME_MIGRATION_ROLLBACK_COMPLETE : RUNTIME_MIGRATION_ROLLBACK_VERIFICATION_FAILED;
}

void runtime_migration_checkpoint_release(RuntimeMigrationCheckpoint *checkpoint)
{
    if (checkpoint != NULL)
        memset(checkpoint, 0, sizeof(*checkpoint));
}
