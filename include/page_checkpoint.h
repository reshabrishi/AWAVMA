#ifndef AWAVMA_PAGE_CHECKPOINT_H
#define AWAVMA_PAGE_CHECKPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define PAGE_CHECKPOINT_MAX_PAGES 4096U

typedef enum {
    PAGE_CHECKPOINT_COMPLETE,
    PAGE_ADDRESS_SET_UNAVAILABLE,
    PAGE_ADDRESS_INVALID,
    PAGE_ADDRESS_DUPLICATE,
    PAGE_QUERY_PERMISSION_DENIED,
    PAGE_QUERY_PARTIAL,
    PAGE_QUERY_FAILED,
    PAGE_CHECKPOINT_INCOMPLETE,
    PAGE_TARGET_GONE,
    PAGE_IDENTITY_CHANGED,
    PAGE_CHECKPOINT_ALLOCATION_FAILED,
    PAGE_CHECKPOINT_TOO_LARGE,
    PAGE_CHECKPOINT_STALE
} PageCheckpointResult;

typedef struct {
    void *address;
    bool address_valid;
    bool original_node_known;
    int original_node;
    int query_status;
} PageCheckpointEntry;

typedef struct {
    pid_t pid;
    uint64_t start_time_ticks;
    char attempt_id[128];
    struct timespec captured_at;
    size_t system_page_size;
    size_t requested_count;
    size_t known_count;
    size_t unknown_count;
    bool complete;
    PageCheckpointResult result;
    PageCheckpointEntry *entries;
} MigrationPageCheckpoint;

/* Address-free summary for a future memory-action recovery eligibility check. */
typedef struct {
    pid_t pid;
    uint64_t start_time_ticks;
    char attempt_id[128];
    size_t candidate_count;
    bool checkpoint_complete;
    bool original_placement_known;
    bool rollback_provider_retained;
    int source_numa_node;
    int target_numa_node;
} MemoryRecoveryEvidence;

typedef int (*page_checkpoint_query_fn)(void *context, pid_t pid, void **pages,
                                         size_t page_count, int *status);

typedef struct {
    pid_t pid;
    uint64_t start_time_ticks;
    const char *attempt_id;
    const void *const *pages;
    size_t page_count;
    bool authoritative_address_set;
    /* Test adapters may provide a query implementation; production uses move_pages query mode. */
    page_checkpoint_query_fn query_fn;
    void *query_context;
} PageCheckpointRequest;

const char *page_checkpoint_result_name(PageCheckpointResult result);
PageCheckpointResult page_checkpoint_capture(const PageCheckpointRequest *request,
                                              MigrationPageCheckpoint *checkpoint);
bool page_checkpoint_matches_attempt(const MigrationPageCheckpoint *checkpoint, pid_t pid,
                                     uint64_t start_time_ticks, const char *attempt_id);
void page_checkpoint_release(MigrationPageCheckpoint *checkpoint);

/* Builds an immutable-by-value summary; it never copies page addresses. */
bool page_checkpoint_recovery_evidence(const MigrationPageCheckpoint *checkpoint,
                                       bool rollback_provider_retained,
                                       int source_numa_node, int target_numa_node,
                                       MemoryRecoveryEvidence *evidence);

#endif
