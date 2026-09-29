#define _POSIX_C_SOURCE 200809L
#include "page_rollback.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { int verify[3]; int move_result; int query_result; unsigned move_calls; } fixture_t;

static uint64_t ticks(void)
{
    char line[4096], *cursor, *save = NULL; FILE *file = fopen("/proc/self/stat", "r");
    if (file == NULL || fgets(line, sizeof(line), file) == NULL) { if (file) fclose(file); return 0; }
    fclose(file); cursor = strrchr(line, ')'); if (cursor == NULL) return 0;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor + 2 : NULL, " ", &save);
        if (token == NULL) return 0;
        if (field == 22) return strtoull(token, NULL, 10);
    }
    return 0;
}

static int move(void *context, pid_t pid, void **pages, const int *nodes, size_t count, int *status)
{
    fixture_t *fixture = context; (void)pid; (void)pages; (void)nodes;
    fixture->move_calls++; for (size_t i = 0; i < count; i++) status[i] = 0;
    return fixture->move_result;
}
static int query(void *context, pid_t pid, void **pages, size_t count, int *status)
{
    fixture_t *fixture = context; (void)pid; (void)pages;
    for (size_t i = 0; i < count; i++) status[i] = fixture->verify[i];
    return fixture->query_result;
}
static void report(const char *name, bool passed) { printf("%s: %s\n", name, passed ? "PASS" : "FAIL"); }

static MigrationPageCheckpoint checkpoint_for(uint64_t start, bool complete)
{
    MigrationPageCheckpoint checkpoint = {0};
    checkpoint.pid = getpid(); checkpoint.start_time_ticks = start; checkpoint.complete = complete;
    checkpoint.requested_count = 3; checkpoint.known_count = complete ? 3 : 2;
    snprintf(checkpoint.attempt_id, sizeof(checkpoint.attempt_id), "rollback-attempt");
    checkpoint.entries = calloc(3, sizeof(*checkpoint.entries));
    for (size_t i = 0; i < 3; i++) {
        checkpoint.entries[i].address = (void *)(uintptr_t)(0x1000 + i * 0x1000);
        checkpoint.entries[i].address_valid = true; checkpoint.entries[i].original_node_known = true;
        checkpoint.entries[i].original_node = i == 1 ? 1 : 0;
    }
    return checkpoint;
}

int main(void)
{
    uint64_t start = ticks(); MigrationPageCheckpoint checkpoint = checkpoint_for(start, true);
    PageRollbackRequest request = {.checkpoint = &checkpoint, .pid = getpid(), .start_time_ticks = start,
                                   .attempt_id = "rollback-attempt", .move_fn = move, .query_fn = query};
    PageRollbackSummary summary; fixture_t fixture = {.verify = {0, 1, 0}}; bool passed = start != 0;
    request.context = &fixture;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_COMPLETE &&
             summary.requested_count == 3 && summary.restored_count == 3 && fixture.move_calls == 1;
    report("PR01_COMPLETE", passed);
    fixture = (fixture_t){.verify = {0, 1, -EFAULT}}; request.context = &fixture;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_PARTIAL &&
             summary.restored_count == 2 && summary.failed_count == 1;
    report("PR02_PARTIAL", passed);
    fixture = (fixture_t){.verify = {-EFAULT, -EFAULT, -EFAULT}}; request.context = &fixture;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_FAILED &&
             summary.restored_count == 0;
    report("PR03_FAILED", passed);
    checkpoint.complete = false; fixture.move_calls = 0;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE &&
             fixture.move_calls == 0;
    report("PR04_INCOMPLETE_CHECKPOINT", passed); checkpoint.complete = true;
    request.start_time_ticks = start + 1; fixture.move_calls = 0;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_IDENTITY_CHANGED &&
             fixture.move_calls == 0;
    report("PR05_IDENTITY_MISMATCH", passed); request.start_time_ticks = start;
    request.attempt_id = "other"; fixture.move_calls = 0;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_ATTEMPT_MISMATCH &&
             fixture.move_calls == 0;
    report("PR07_ATTEMPT_MISMATCH", passed); request.attempt_id = "rollback-attempt";
    checkpoint.entries[0].original_node = 0; checkpoint.entries[0].original_node_known = true;
    fixture = (fixture_t){.verify = {0, 1, 0}}; request.context = &fixture;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_COMPLETE;
    checkpoint.entries[0].original_node_known = false; fixture.move_calls = 0;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE &&
             fixture.move_calls == 0;
    report("PR08_NODE_ZERO_VALID", passed); checkpoint.entries[0].original_node_known = true;
    fixture = (fixture_t){.verify = {0, 1, 0}, .query_result = -EIO}; request.context = &fixture;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_VERIFICATION_FAILED;
    report("PR09_VERIFY_QUERY_FAILURE", passed);
    fixture = (fixture_t){.move_result = -EPERM}; request.context = &fixture;
    passed = passed && page_rollback_restore(&request, &summary) == PAGE_ROLLBACK_PERMISSION_DENIED;
    report("PR10_PERMISSION_DENIED", passed);
    page_checkpoint_release(&checkpoint);
    report("PR12_CHECKPOINT_IMMUTABLE", checkpoint.entries == NULL);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
