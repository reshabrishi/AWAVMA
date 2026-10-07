#define _POSIX_C_SOURCE 200809L
#include "thread_candidate.h"
#include "runtime_migration_metadata.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool task_start(pid_t pid, pid_t tid, uint64_t *ticks)
{
    char path[96], line[4096], *cursor, *save = NULL;
    FILE *file;
    if (snprintf(path, sizeof(path), "/proc/%ld/task/%ld/stat", (long)pid, (long)tid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) fclose(file);
        return false;
    }
    fclose(file);
    cursor = strrchr(line, ')');
    if (cursor == NULL || cursor[1] != ' ') return false;
    cursor += 2;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor : NULL, " ", &save);
        char *end = NULL;
        unsigned long long value;
        if (token == NULL) return false;
        if (field != 22) continue;
        errno = 0; value = strtoull(token, &end, 10);
        if (errno != 0 || end == token) return false;
        *ticks = (uint64_t)value;
        return true;
    }
    return false;
}

static bool valid_task(pid_t pid, pid_t tid, uint64_t process_ticks, thread_candidate_t *candidate)
{
    RuntimeMigrationMetadata metadata;
    cpu_set_t affinity;
    uint64_t ticks;
    if (!runtime_get_migration_metadata(pid, process_ticks, &metadata) || !metadata.process_exists ||
        !metadata.identity_match || tid <= 0 || sched_getaffinity(tid, sizeof(affinity), &affinity) != 0 ||
        CPU_COUNT(&affinity) == 0 || !task_start(pid, tid, &ticks)) return false;
    candidate->tid = tid; candidate->start_time_ticks = ticks; candidate->start_time_ticks_available = true;
    candidate->verified = true;
    snprintf(candidate->provenance, sizeof(candidate->provenance), "PROC_TASK_AFFINITY");
    return true;
}

bool thread_candidate_select(pid_t pid, uint64_t process_start_time_ticks, thread_candidate_t *candidate)
{
    DIR *directory;
    struct dirent *entry;
    pid_t leader = pid, selected = -1;
    thread_candidate_t result = {0};
    if (candidate == NULL || pid <= 0 || process_start_time_ticks == 0) return false;
    memset(candidate, 0, sizeof(*candidate));
    char path[64];
    if (snprintf(path, sizeof(path), "/proc/%ld/task", (long)pid) >= (int)sizeof(path) ||
        (directory = opendir(path)) == NULL) return false;
    while ((entry = readdir(directory)) != NULL) {
        char *end = NULL; long value;
        if (entry->d_name[0] == '.') continue;
        errno = 0; value = strtol(entry->d_name, &end, 10);
        if (errno != 0 || *end != '\0' || value <= 0 || value > INT_MAX || value == leader) continue;
        if (selected < 0 || value < selected) selected = (pid_t)value;
    }
    closedir(directory);
    if (selected > 0 && valid_task(pid, selected, process_start_time_ticks, &result)) { *candidate = result; return true; }
    return valid_task(pid, leader, process_start_time_ticks, candidate);
}
