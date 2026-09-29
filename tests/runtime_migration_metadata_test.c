#define _POSIX_C_SOURCE 200809L

#include "runtime_migration_metadata.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void report(const char *id, int passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
}

static uint64_t start_ticks(pid_t pid)
{
    char path[64];
    char line[4096];
    char *cursor;
    char *save = NULL;
    FILE *file;

    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL)
            fclose(file);
        return 0;
    }
    fclose(file);
    cursor = strrchr(line, ')');
    if (cursor == NULL || cursor[1] != ' ')
        return 0;
    cursor += 2;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor : NULL, " ", &save);

        if (token == NULL)
            return 0;
        if (field == 22)
            return strtoull(token, NULL, 10);
    }
    return 0;
}

int main(void)
{
    RuntimeMigrationMetadata metadata;
    RuntimeMigrationCheckpoint checkpoint;
    RuntimeMigrationCheckpoint mismatch;
    cpu_set_t initial;
    cpu_set_t changed;
    cpu_set_t verified;
    pid_t child;
    uint64_t ticks;
    int first = -1;
    int second = -1;
    int third = -1;
    int passed = 1;

    child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    if (child < 0)
        return EXIT_FAILURE;
    usleep(10000);
    ticks = start_ticks(child);
    passed = ticks != 0 && sched_getaffinity(child, sizeof(initial), &initial) == 0;
    changed = initial;
    report("MD01_LIVE_PID", passed);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &initial)) {
            if (first < 0)
                first = cpu;
            else if (second < 0)
                second = cpu;
            else if (third < 0)
                third = cpu;
        }
    if (second >= 0) {
        CPU_ZERO(&changed);
        CPU_SET(first, &changed);
        CPU_SET(third >= 0 ? third : second, &changed);
        passed = passed && sched_setaffinity(child, sizeof(changed), &changed) == 0;
    }
    passed = passed && runtime_get_migration_metadata(child, ticks, &metadata) &&
             metadata.process_exists && metadata.identity_match && metadata.affinity_available &&
             CPU_EQUAL(&metadata.affinity, &changed) && !metadata.page_placement_available;
    report("MD02_IDENTITY_AND_AFFINITY", passed);
    passed = passed && runtime_migration_checkpoint_affinity(&metadata, &checkpoint) &&
             CPU_EQUAL(&checkpoint.original_affinity, &changed);
    report("C01_BINARY_CHECKPOINT", passed);
    if (first >= 0) {
        CPU_ZERO(&changed);
        CPU_SET(first, &changed);
        passed = passed && sched_setaffinity(child, sizeof(changed), &changed) == 0 &&
                 runtime_migration_restore_affinity(&checkpoint) == RUNTIME_MIGRATION_ROLLBACK_COMPLETE &&
                 sched_getaffinity(child, sizeof(verified), &verified) == 0 &&
                 CPU_EQUAL(&verified, &checkpoint.original_affinity);
    }
    report("C04_REAL_CPU_AFFINITY_ROLLBACK", passed);
    mismatch = checkpoint;
    mismatch.start_time_ticks++;
    passed = passed && runtime_migration_restore_affinity(&mismatch) ==
             RUNTIME_MIGRATION_ROLLBACK_IDENTITY_CHANGED;
    report("C05_IDENTITY_MISMATCH", passed);
    runtime_migration_checkpoint_release(&checkpoint);
    passed = passed && !checkpoint.affinity_available && checkpoint.pid == 0;
    report("C03_CHECKPOINT_RELEASE", passed);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    passed = passed && runtime_get_migration_metadata(child, ticks, &metadata) &&
             !metadata.process_exists && !metadata.identity_match;
    report("MD04_PROCESS_GONE", passed);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
