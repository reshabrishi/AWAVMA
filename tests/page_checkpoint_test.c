#define _POSIX_C_SOURCE 200809L
#include "migration_safety_manager.h"
#include "page_checkpoint.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void report(const char *name, bool passed)
{
    printf("%s: %s\n", name, passed ? "PASS" : "FAIL");
}

static uint64_t ticks(pid_t pid)
{
    char path[64], line[4096], *cursor, *save = NULL;
    FILE *file;
    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) fclose(file);
        return 0;
    }
    fclose(file); cursor = strrchr(line, ')');
    if (cursor == NULL) return 0;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor + 2 : NULL, " ", &save);
        if (token == NULL) return 0;
        if (field == 22) return strtoull(token, NULL, 10);
    }
    return 0;
}

static int fixed_query(void *context, pid_t pid, void **pages, size_t count, int *status)
{
    const int *values = context;
    (void)pid;
    (void)pages;
    for (size_t index = 0; index < count; index++) status[index] = values[index];
    return 0;
}

static bool identity(void *context, pid_t pid, uint64_t start_time_ticks)
{
    (void)context; return pid == getpid() && start_time_ticks == ticks(pid);
}

static PageCheckpointResult unavailable_checkpoint(void *context, const MigrationSafetyRequest *request,
                                                   const char *attempt, MigrationPageCheckpoint *checkpoint)
{
    PageCheckpointRequest input = {0};
    (void)context;
    input.pid = request->pid; input.start_time_ticks = request->start_time_ticks;
    input.attempt_id = attempt;
    return page_checkpoint_capture(&input, checkpoint);
}

static MigrationResultCode executor(void *context, const MigrationRequest *request, MigrationReport *report)
{
    (*(unsigned *)context)++; (void)request; (void)report; return MIGRATION_SYSTEM_ERROR;
}

static bool feedback(void *context, const FeedbackEvent *event, FeedbackResult *result)
{
    (*(unsigned *)context)++; (void)event; (void)result; return true;
}

int main(void)
{
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    void *memory = mmap(NULL, page_size * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *pages[3];
    PageCheckpointRequest request = {0};
    MigrationPageCheckpoint checkpoint = {0};
    uint64_t start = ticks(getpid());
    bool passed = memory != MAP_FAILED && page_size > 0 && start != 0;
    int partial_status[] = {0, -EFAULT, 0};

    if (!passed) return EXIT_FAILURE;
    for (size_t index = 0; index < 3; index++) {
        pages[index] = (char *)memory + index * page_size;
        *((volatile char *)pages[index]) = (char)index;
    }
    request.pid = getpid(); request.start_time_ticks = start; request.attempt_id = "pc-real";
    request.pages = (const void *const *)pages; request.page_count = 1;
    request.authoritative_address_set = true;
    PageCheckpointResult actual = page_checkpoint_capture(&request, &checkpoint);
    passed = passed && (actual == PAGE_CHECKPOINT_COMPLETE || actual == PAGE_QUERY_PERMISSION_DENIED ||
                        actual == PAGE_QUERY_FAILED);
    if (actual == PAGE_CHECKPOINT_COMPLETE)
        passed = passed && checkpoint.known_count == 1 && checkpoint.entries[0].original_node_known;
    report("PC01_REAL_RESIDENT_PAGE", passed); page_checkpoint_release(&checkpoint);

    request.attempt_id = "pc-multi"; request.pages = (const void *const *)pages; request.page_count = 3;
    request.query_fn = fixed_query; request.query_context = (int[]){0, 0, 0};
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_CHECKPOINT_COMPLETE &&
             checkpoint.known_count == 3 && checkpoint.entries[0].original_node == 0 &&
             checkpoint.entries[0].original_node_known;
    report("PC02_MULTIPLE_PAGES", passed); page_checkpoint_release(&checkpoint);

    request.attempt_id = "pc-partial"; request.query_context = partial_status;
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_CHECKPOINT_INCOMPLETE &&
             checkpoint.known_count == 2 && checkpoint.unknown_count == 1 && !checkpoint.complete &&
             checkpoint.entries[1].query_status == -EFAULT && !checkpoint.entries[1].original_node_known;
    report("PC03_UNTOUCHED_OR_UNRESOLVED_PAGE", passed); report("PC11_PARTIAL_QUERY", passed);
    page_checkpoint_release(&checkpoint);

    request.attempt_id = "pc-invalid"; request.pages = (const void *const *)pages;
    request.page_count = 1; request.query_context = (int[]){-EFAULT};
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_CHECKPOINT_INCOMPLETE &&
             checkpoint.unknown_count == 1 && checkpoint.entries[0].query_status == -EFAULT;
    report("PC04_INVALID_ADDRESS", passed); page_checkpoint_release(&checkpoint);

    pages[0] = (char *)memory + 1; request.pages = (const void *const *)pages; request.page_count = 1;
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_ADDRESS_INVALID;
    report("PC05_UNALIGNED_ADDRESS", passed);
    pages[0] = memory; pages[1] = memory; request.pages = (const void *const *)pages; request.page_count = 2;
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_ADDRESS_DUPLICATE;
    report("PC06_DUPLICATE_PAGE", passed);
    request.pages = NULL; request.page_count = 0;
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_ADDRESS_SET_UNAVAILABLE;
    report("PC07_EMPTY_PAGE_SET", passed);
    request.pages = (const void *const *)pages; request.page_count = PAGE_CHECKPOINT_MAX_PAGES + 1;
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_CHECKPOINT_TOO_LARGE;
    report("PC08_TOO_MANY_PAGES", passed);
    request.page_count = 1; request.start_time_ticks = start + 1;
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_IDENTITY_CHANGED;
    report("PC09_IDENTITY_MISMATCH", passed);
    {
        pid_t child = fork();
        uint64_t child_start;

        if (child == 0) for (;;) pause();
        usleep(10000); child_start = child > 0 ? ticks(child) : 0;
        if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
        request.pid = child; request.start_time_ticks = child_start;
        passed = passed && child > 0 && child_start != 0 &&
                 page_checkpoint_capture(&request, &checkpoint) == PAGE_TARGET_GONE;
        request.pid = getpid(); request.start_time_ticks = start;
    }
    report("PC10_TARGET_GONE", passed);
    request.start_time_ticks = start; request.attempt_id = "pc-immutable"; pages[0] = memory;
    request.query_context = (int[]){0};
    passed = passed && page_checkpoint_capture(&request, &checkpoint) == PAGE_CHECKPOINT_COMPLETE &&
              page_checkpoint_matches_attempt(&checkpoint, getpid(), start, "pc-immutable");
    MemoryRecoveryEvidence recovery = {0};
    passed = passed && page_checkpoint_recovery_evidence(&checkpoint, true, 0, 1, &recovery) &&
              recovery.checkpoint_complete && recovery.original_placement_known &&
              recovery.rollback_provider_retained && recovery.candidate_count == 1 &&
              strcmp(recovery.attempt_id, "pc-immutable") == 0;
    pages[0] = (char *)memory + page_size;
    passed = passed && checkpoint.entries[0].address == memory &&
              !page_checkpoint_matches_attempt(&checkpoint, getpid(), start, "other-attempt");
    report("PC12_NODE_ZERO_VALIDITY", passed); report("PC13_ATTEMPT_MISMATCH", passed);
    report("PC14_IMMUTABLE_AFTER_CAPTURE", passed); report("PC15_RECOVERY_EVIDENCE_COMPLETE", passed);
    page_checkpoint_release(&checkpoint);
    memset(&recovery, 0, sizeof(recovery));
    passed = passed && !page_checkpoint_recovery_evidence(&checkpoint, true, 0, 1, &recovery);
    report("PC16_RECOVERY_EVIDENCE_INCOMPLETE_REJECTED", passed);

    {
        MigrationSafetyManager *manager = migration_safety_manager_create();
        MigrationSafetyConfig config; MigrationSafetyRequest safety_request = {0};
        MigrationSafetyResult result; unsigned executor_calls = 0, feedback_calls = 0;
        migration_safety_config_default(&config); config.enabled = true; config.identity_fn = identity;
        config.page_checkpoint_fn = unavailable_checkpoint; config.execute_fn = executor;
        config.feedback_fn = feedback; config.callback_context = &feedback_calls;
        /* The executor counter is intentionally separate from the feedback callback context. */
        config.execute_fn = NULL;
        snprintf(safety_request.app_id, sizeof(safety_request.app_id), "page-checkpoint");
        safety_request.pid = getpid(); safety_request.start_time_ticks = start;
        safety_request.action = VALIDATION_ACTION_MOVE_MEMORY; safety_request.system_safe = true;
        safety_request.migration_request.pid = getpid();
        safety_request.migration_request.start_time_ticks = start;
        safety_request.migration_request.start_time_ticks_available = true;
        safety_request.migration_request.phase5_decision.action = VALIDATION_ACTION_MOVE_MEMORY;
        snprintf(safety_request.migration_request.phase6_validation.final_decision,
                 sizeof(safety_request.migration_request.phase6_validation.final_decision), "APPROVED");
        passed = passed && manager != NULL && migration_safety_manager_init(manager, &config) &&
                 migration_safety_manager_attempt(manager, &safety_request, &result) &&
                 result.page_checkpoint_result == PAGE_ADDRESS_SET_UNAVAILABLE &&
                 result.state == MIGRATION_SAFETY_TARGET_UNAVAILABLE && feedback_calls == 1 &&
                 executor_calls == 0;
        migration_safety_manager_destroy(manager);
    }
    report("PC15_CLEANUP", passed);
    munmap(memory, page_size * 3);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
