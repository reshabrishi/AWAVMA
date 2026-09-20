#ifndef AWAVMA_PAGE_ROLLBACK_H
#define AWAVMA_PAGE_ROLLBACK_H

#include "page_checkpoint.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PAGE_ROLLBACK_COMPLETE,
    PAGE_ROLLBACK_PARTIAL,
    PAGE_ROLLBACK_FAILED,
    PAGE_ROLLBACK_UNAVAILABLE,
    PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE,
    PAGE_ROLLBACK_TARGET_GONE,
    PAGE_ROLLBACK_IDENTITY_CHANGED,
    PAGE_ROLLBACK_ATTEMPT_MISMATCH,
    PAGE_ROLLBACK_QUERY_FAILED,
    PAGE_ROLLBACK_VERIFICATION_FAILED,
    PAGE_ROLLBACK_PERMISSION_DENIED,
    PAGE_ROLLBACK_ALLOCATION_FAILED
} PageRollbackResult;

typedef int (*page_rollback_move_fn)(void *context, pid_t pid, void **pages, const int *nodes,
                                      size_t page_count, int *status);

typedef struct {
    const MigrationPageCheckpoint *checkpoint;
    pid_t pid;
    uint64_t start_time_ticks;
    const char *attempt_id;
    page_rollback_move_fn move_fn;
    page_checkpoint_query_fn query_fn;
    void *context;
} PageRollbackRequest;

typedef struct {
    PageRollbackResult result;
    size_t requested_count;
    size_t restored_count;
    size_t failed_count;
    size_t verified_count;
    bool attempted;
} PageRollbackSummary;

const char *page_rollback_result_name(PageRollbackResult result);
PageRollbackResult page_rollback_restore(const PageRollbackRequest *request,
                                         PageRollbackSummary *summary);

#ifdef AWAVMA_RUNTIME_TESTING
typedef struct {
    int move_result;
    int query_result;
    const int *query_status;
    size_t query_status_count;
    bool force_identity_changed;
    unsigned provider_calls;
    unsigned move_calls;
    unsigned query_calls;
} PageRollbackTestAdapter;

void page_rollback_test_adapter_set(PageRollbackTestAdapter *adapter);
void page_rollback_test_adapter_reset(void);
#endif

#endif
