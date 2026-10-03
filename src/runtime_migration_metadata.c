#define _GNU_SOURCE
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

static bool parse_cpu_list(const char *text, cpu_set_t *set)
{
    const char *cursor = text;

    CPU_ZERO(set);
    while (*cursor != '\0' && *cursor != '\n') {
        char *end;
        long first = strtol(cursor, &end, 10);
        long last = first;

        if (end == cursor || first < 0 || first >= CPU_SETSIZE)
            return false;
        if (*end == '-') {
            last = strtol(end + 1, &end, 10);
            if (last < first || last >= CPU_SETSIZE)
                return false;
        }
        for (long cpu = first; cpu <= last; cpu++)
            CPU_SET((int)cpu, set);
        if (*end == ',')
            cursor = end + 1;
        else if (*end == '\0' || *end == '\n')
            break;
        else
            return false;
    }
    return CPU_COUNT(set) > 0;
}

static bool read_cpu_set_file(const char *path, cpu_set_t *set)
{
    char line[4096];
    FILE *file;

    file = fopen(path, "r");
    if (file == NULL)
        return false;
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }
    fclose(file);
    return line[0] != '\n' && line[0] != '\0' && parse_cpu_list(line, set);
}

static bool read_cgroup_cpuset(pid_t pid, RuntimeMigrationMetadata *metadata)
{
    char proc_path[64], line[4096], group_path[256] = {0}, path[512];
    FILE *file;
    unsigned version = 0;

    if (snprintf(proc_path, sizeof(proc_path), "/proc/%ld/cgroup", (long)pid) >= (int)sizeof(proc_path))
        return false;
    file = fopen(proc_path, "r");
    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *first = strchr(line, ':');
        char *second = first == NULL ? NULL : strchr(first + 1, ':');

        if (first == NULL || second == NULL)
            continue;
        *second++ = '\0';
        second[strcspn(second, "\r\n")] = '\0';
        if (first[1] == '\0') {
            version = 2;
            snprintf(group_path, sizeof(group_path), "%s", second);
            break;
        }
        if (strstr(first + 1, "cpuset") != NULL) {
            version = 1;
            snprintf(group_path, sizeof(group_path), "%s", second);
            break;
        }
    }
    fclose(file);
    if (version == 0 || group_path[0] != '/')
        return false;
    metadata->cgroup_version = version;
    snprintf(metadata->cgroup_path, sizeof(metadata->cgroup_path), "%s", group_path);
    if (version == 2) {
        char directory[512];

        if (snprintf(directory, sizeof(directory), "/sys/fs/cgroup%s", group_path) >= (int)sizeof(directory))
            return false;
        for (;;) {
            if (snprintf(path, sizeof(path), "%s/cpuset.cpus.effective", directory) >= (int)sizeof(path))
                return false;
            if (access(path, F_OK) == 0) {
                if (!read_cpu_set_file(path, &metadata->permitted_cpu_set))
                    return false;
                snprintf(metadata->cpuset_source_path, sizeof(metadata->cpuset_source_path), "%s", path);
                return true;
            }
            if (strcmp(directory, "/sys/fs/cgroup") == 0)
                return false;
            char *slash = strrchr(directory, '/');
            if (slash == NULL || slash == directory)
                return false;
            *slash = '\0';
        }
    }
    if (snprintf(path, sizeof(path), "/sys/fs/cgroup/cpuset%s/cpuset.cpus", group_path) >= (int)sizeof(path))
        return false;
    if (!read_cpu_set_file(path, &metadata->permitted_cpu_set))
        return false;
    snprintf(metadata->cpuset_source_path, sizeof(metadata->cpuset_source_path), "%s", path);
    return true;
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
    metadata->tid = pid;
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
    metadata->permitted_cpu_set_available = read_cgroup_cpuset(pid, metadata);
    /* The runtime has no node-of-CPU provider without libnuma; do not infer node zero. */
    return true;
}

bool runtime_get_process_start_time_ticks(pid_t pid, uint64_t *start_time_ticks)
{
    uint64_t ignored_cpu_ticks;
    int ignored_cpu;

    return start_time_ticks != NULL && read_stat(pid, start_time_ticks, &ignored_cpu_ticks, &ignored_cpu);
}

bool runtime_get_thread_migration_metadata(pid_t pid, pid_t tid, uint64_t start_time_ticks,
                                            RuntimeMigrationMetadata *metadata)
{
    RuntimeMigrationMetadata process;
    uint64_t thread_start = 0, cpu_ticks = 0;
    int cpu = -1;
    char path[160];

    if (metadata == NULL || tid <= 0 ||
        !runtime_get_migration_metadata(pid, start_time_ticks, &process))
        return false;
    *metadata = process;
    metadata->tid = tid;
    if (!process.identity_match ||
        snprintf(path, sizeof(path), "/proc/%ld/task/%ld", (long)pid, (long)tid) >= (int)sizeof(path))
        return true;
    metadata->thread_exists = access(path, F_OK) == 0;
    metadata->thread_belongs_to_process = metadata->thread_exists;
    if (!metadata->thread_belongs_to_process || !read_stat(tid, &thread_start, &cpu_ticks, &cpu))
        return true;
    metadata->thread_start_time_ticks = thread_start;
    metadata->thread_start_time_ticks_available = true;
    metadata->current_cpu_available = cpu >= 0;
    metadata->current_cpu = cpu;
    metadata->process_cpu_time_available = true;
    metadata->process_cpu_time_ticks = cpu_ticks;
    metadata->affinity_available = sched_getaffinity(tid, sizeof(metadata->affinity), &metadata->affinity) == 0;
    metadata->thread_metadata_available = metadata->current_cpu_available && metadata->affinity_available;
    return true;
}

bool runtime_migration_checkpoint_affinity(const RuntimeMigrationMetadata *metadata,
                                           RuntimeMigrationCheckpoint *checkpoint)
{
    if (checkpoint == NULL)
        return false;
    memset(checkpoint, 0, sizeof(*checkpoint));
    if (metadata == NULL || !metadata->identity_match || !metadata->thread_start_time_ticks_available ||
        !metadata->affinity_available)
        return false;
    checkpoint->captured_at_ms = monotonic_ms();
    checkpoint->pid = metadata->pid;
    checkpoint->tid = metadata->tid;
    checkpoint->start_time_ticks = metadata->start_time_ticks;
    checkpoint->thread_start_time_ticks = metadata->thread_start_time_ticks;
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
    if (!runtime_get_thread_migration_metadata(checkpoint->pid, checkpoint->tid,
                                               checkpoint->start_time_ticks, &current))
        return RUNTIME_MIGRATION_ROLLBACK_FAILED;
    if (!current.process_exists)
        return RUNTIME_MIGRATION_ROLLBACK_TARGET_GONE;
    if (!current.identity_match || !current.thread_belongs_to_process ||
        !current.thread_start_time_ticks_available ||
        current.thread_start_time_ticks != checkpoint->thread_start_time_ticks)
        return RUNTIME_MIGRATION_ROLLBACK_IDENTITY_CHANGED;
    if (sched_setaffinity(checkpoint->tid, sizeof(checkpoint->original_affinity),
                          &checkpoint->original_affinity) != 0)
        return RUNTIME_MIGRATION_ROLLBACK_FAILED;
    if (sched_getaffinity(checkpoint->tid, sizeof(restored), &restored) != 0)
        return RUNTIME_MIGRATION_ROLLBACK_VERIFICATION_FAILED;
    return CPU_EQUAL(&restored, &checkpoint->original_affinity) ?
        RUNTIME_MIGRATION_ROLLBACK_COMPLETE : RUNTIME_MIGRATION_ROLLBACK_VERIFICATION_FAILED;
}

void runtime_migration_checkpoint_release(RuntimeMigrationCheckpoint *checkpoint)
{
    if (checkpoint != NULL)
        memset(checkpoint, 0, sizeof(*checkpoint));
}
