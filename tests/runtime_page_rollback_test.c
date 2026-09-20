#define _POSIX_C_SOURCE 200809L
#include "awavma_runtime.h"
#include "page_rollback.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t start_ticks(pid_t pid)
{
    char path[64], line[4096], *cursor, *save = NULL; FILE *file;
    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) return 0;
    fclose(file); cursor = strrchr(line, ')'); if (cursor == NULL) return 0;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor + 2 : NULL, " ", &save);
        if (token == NULL) return 0;
        if (field == 22) return strtoull(token, NULL, 10);
    }
    return 0;
}

int main(void)
{
    char root[] = "/tmp/awavma-runtime-page-rollback-XXXXXX", cwd[PATH_MAX], bin_dir[PATH_MAX + 32];
    char config_path[PATH_MAX + 32], feedback_path[PATH_MAX + 256], line[4096];
    awavma_runtime_config_t config; awavma_runtime_t *runtime; awavma_runtime_test_target_stats_t stats;
    MigrationPageCheckpoint checkpoint = {0}; PageCheckpointEntry entries[4] = {0};
    PageRollbackTestAdapter adapter = {0}; int nodes[4] = {0, 0, 0, 0};
    pid_t child; uint64_t ticks; long page_size; void *pages; FILE *file; unsigned rows = 0; bool passed, partial, failed, verify_failed, identity, mismatch, incomplete;
    if (mkdtemp(root) == NULL || getcwd(cwd, sizeof(cwd)) == NULL) return EXIT_FAILURE;
    child = fork(); if (child == 0) for (;;) pause(); if (child < 0) return EXIT_FAILURE;
    usleep(10000); ticks = start_ticks(child); page_size = sysconf(_SC_PAGESIZE);
    pages = mmap(NULL, (size_t)page_size * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ticks == 0 || page_size <= 0 || pages == MAP_FAILED) return EXIT_FAILURE;
    checkpoint.pid = child; checkpoint.start_time_ticks = ticks; checkpoint.complete = true;
    checkpoint.requested_count = checkpoint.known_count = 4; checkpoint.entries = entries;
    snprintf(checkpoint.attempt_id, sizeof(checkpoint.attempt_id), "rts01-page-recovery");
    for (size_t i = 0; i < 4; i++) entries[i] = (PageCheckpointEntry){
        .address = (char *)pages + i * page_size, .address_valid = true,
        .original_node = 0, .original_node_known = true };
    adapter.move_result = 0; adapter.query_result = 0; adapter.query_status = nodes; adapter.query_status_count = 4;
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cwd); snprintf(config_path, sizeof(config_path), "%s/config/awavma.conf", cwd);
    snprintf(feedback_path, sizeof(feedback_path), "%s/apps/target-integration/history/migration_feedback.csv", root);
    awavma_runtime_config_default(&config); config.root_dir = root; config.bin_dir = bin_dir;
    config.phase_config_path = config_path; config.migration_safety_enabled = true;
    runtime = awavma_runtime_create(); page_rollback_test_adapter_set(&adapter);
    passed = runtime != NULL && awavma_runtime_init(runtime, &config) == 0 &&
        awavma_runtime_test_resume_page_recovery(runtime, checkpoint.attempt_id, &checkpoint, &stats) == 0 &&
        stats.rollback_calls == 1 && stats.terminal_feedback_calls == 1 && adapter.move_calls == 1 && adapter.query_calls == 1;
    file = fopen(feedback_path, "r");
    while (file != NULL && fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, "rts01-page-recovery") != NULL && strstr(line, "PAGE_ROLLBACK_COMPLETE") != NULL) rows++;
    if (file != NULL) fclose(file);
    passed = passed && rows == 1;
    nodes[3] = 1; memset(&adapter, 0, sizeof(adapter)); adapter.query_status = nodes; adapter.query_status_count = 4;
    page_rollback_test_adapter_set(&adapter); snprintf(checkpoint.attempt_id, sizeof(checkpoint.attempt_id), "rpr02-partial");
    partial = awavma_runtime_test_resume_page_recovery(runtime, checkpoint.attempt_id, &checkpoint, &stats) == 0 && stats.rollback_calls == 1 && stats.terminal_feedback_calls == 1 && adapter.provider_calls == 1 && adapter.move_calls == 1 && adapter.query_calls == 1;
    rows = 0; file = fopen(feedback_path, "r"); while (file != NULL && fgets(line, sizeof(line), file) != NULL) if (strstr(line, "rpr02-partial") && strstr(line, "PAGE_ROLLBACK_PARTIAL")) rows++; if (file) fclose(file); partial = partial && rows == 1;
    memset(nodes, 1, sizeof(nodes)); memset(&adapter, 0, sizeof(adapter)); adapter.query_status = nodes; adapter.query_status_count = 4;
    page_rollback_test_adapter_set(&adapter); snprintf(checkpoint.attempt_id, sizeof(checkpoint.attempt_id), "rpr03-failed");
    failed = awavma_runtime_test_resume_page_recovery(runtime, checkpoint.attempt_id, &checkpoint, &stats) == 0 && stats.rollback_calls == 1 && stats.terminal_feedback_calls == 1 && adapter.provider_calls == 1 && adapter.move_calls == 1 && adapter.query_calls == 1;
    rows = 0; file = fopen(feedback_path, "r"); while (file != NULL && fgets(line, sizeof(line), file) != NULL) if (strstr(line, "rpr03-failed") && strstr(line, "PAGE_ROLLBACK_FAILED")) rows++; if (file) fclose(file); failed = failed && rows == 1;
    memset(&adapter, 0, sizeof(adapter)); adapter.query_result = -EIO; page_rollback_test_adapter_set(&adapter); snprintf(checkpoint.attempt_id, sizeof(checkpoint.attempt_id), "rpr04-verify");
    verify_failed = awavma_runtime_test_resume_page_recovery(runtime, checkpoint.attempt_id, &checkpoint, &stats) == 0 && stats.rollback_calls == 1 && stats.terminal_feedback_calls == 1 && adapter.provider_calls == 1 && adapter.move_calls == 1 && adapter.query_calls == 1;
    rows = 0; file = fopen(feedback_path, "r"); while (file != NULL && fgets(line, sizeof(line), file) != NULL) if (strstr(line, "rpr04-verify") && strstr(line, "PAGE_ROLLBACK_VERIFICATION_FAILED")) rows++; if (file) fclose(file); verify_failed = verify_failed && rows == 1;
    memset(&adapter, 0, sizeof(adapter)); adapter.force_identity_changed = true; page_rollback_test_adapter_set(&adapter); snprintf(checkpoint.attempt_id, sizeof(checkpoint.attempt_id), "rpr05-identity");
    identity = awavma_runtime_test_resume_page_recovery(runtime, checkpoint.attempt_id, &checkpoint, &stats) == 0 && stats.rollback_calls == 1 && stats.terminal_feedback_calls == 1 && adapter.provider_calls == 1 && adapter.move_calls == 0 && adapter.query_calls == 0;
    rows = 0; file = fopen(feedback_path, "r"); while (file != NULL && fgets(line, sizeof(line), file) != NULL) if (strstr(line, "rpr05-identity") && strstr(line, "PAGE_ROLLBACK_IDENTITY_CHANGED")) rows++; if (file) fclose(file); identity = identity && rows == 1;
    page_rollback_test_adapter_reset();
    snprintf(checkpoint.attempt_id, sizeof(checkpoint.attempt_id), "rpr06-checkpoint-attempt");
    memset(&adapter, 0, sizeof(adapter)); page_rollback_test_adapter_set(&adapter);
    mismatch = awavma_runtime_test_resume_page_recovery(runtime, "rpr06-active-attempt", &checkpoint, &stats) == 0 &&
        stats.rollback_calls == 0 && stats.terminal_feedback_calls == 1 && adapter.move_calls == 0 && adapter.query_calls == 0;
    rows = 0; file = fopen(feedback_path, "r");
    while (file != NULL && fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, "rpr06-active-attempt") != NULL && strstr(line, "PAGE_ROLLBACK_ATTEMPT_MISMATCH") != NULL) rows++;
    if (file != NULL)
        fclose(file);
    mismatch = mismatch && rows == 1;
    page_rollback_test_adapter_reset();
    snprintf(checkpoint.attempt_id, sizeof(checkpoint.attempt_id), "rpr07-incomplete"); checkpoint.complete = false;
    memset(&adapter, 0, sizeof(adapter)); page_rollback_test_adapter_set(&adapter);
    incomplete = awavma_runtime_test_resume_page_recovery(runtime, checkpoint.attempt_id, &checkpoint, &stats) == 0 &&
        stats.rollback_calls == 0 && stats.terminal_feedback_calls == 1 && adapter.move_calls == 0 && adapter.query_calls == 0;
    rows = 0; file = fopen(feedback_path, "r");
    while (file != NULL && fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, "rpr07-incomplete") != NULL && strstr(line, "PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE") != NULL) rows++;
    if (file != NULL)
        fclose(file);
    incomplete = incomplete && rows == 1;
    printf("RTS01_POST_CHECKPOINT_RECOVERY: %s\n", passed ? "PASS" : "FAIL");
    printf("RPR02_PARTIAL: %s\n", partial ? "PASS" : "FAIL");
    printf("RPR03_FAILED: %s\n", failed ? "PASS" : "FAIL");
    printf("RPR04_VERIFICATION_FAILURE: %s\n", verify_failed ? "PASS" : "FAIL");
    printf("RPR05_IDENTITY_MISMATCH: %s\n", identity ? "PASS" : "FAIL");
    printf("RPR06_ATTEMPT_MISMATCH: %s\n", mismatch ? "PASS" : "FAIL");
    printf("RPR07_INCOMPLETE_CHECKPOINT: %s\n", incomplete ? "PASS" : "FAIL");
    page_rollback_test_adapter_reset(); awavma_runtime_destroy(runtime); munmap(pages, (size_t)page_size * 4);
    kill(child, SIGTERM); waitpid(child, NULL, 0); return passed && partial && failed && verify_failed && identity && mismatch && incomplete ? EXIT_SUCCESS : EXIT_FAILURE;
}
