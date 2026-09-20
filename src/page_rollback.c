#include "page_rollback.h"

#include "runtime_migration_metadata.h"

#include <errno.h>
#include <linux/mempolicy.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef AWAVMA_RUNTIME_TESTING
static PageRollbackTestAdapter *test_adapter;

void page_rollback_test_adapter_set(PageRollbackTestAdapter *adapter) { test_adapter = adapter; }
void page_rollback_test_adapter_reset(void) { test_adapter = NULL; }
#endif

static int production_move(void *context, pid_t pid, void **pages, const int *nodes,
                           size_t count, int *status)
{
    (void)context;
#ifdef AWAVMA_RUNTIME_TESTING
    if (test_adapter != NULL) {
        test_adapter->move_calls++;
        return test_adapter->move_result;
    }
#endif
#ifdef SYS_move_pages
    return syscall(SYS_move_pages, pid, count, pages, nodes, status, MPOL_MF_MOVE) < 0 ? -errno : 0;
#else
    (void)pid; (void)pages; (void)nodes; (void)count; (void)status;
    return -ENOSYS;
#endif
}

static int production_query(void *context, pid_t pid, void **pages, size_t count, int *status)
{
    (void)context;
#ifdef AWAVMA_RUNTIME_TESTING
    if (test_adapter != NULL) {
        test_adapter->query_calls++;
        if (test_adapter->query_result == 0 && test_adapter->query_status != NULL &&
            test_adapter->query_status_count == count)
            memcpy(status, test_adapter->query_status, count * sizeof(*status));
        return test_adapter->query_result;
    }
#endif
#ifdef SYS_move_pages
    return syscall(SYS_move_pages, pid, count, pages, NULL, status, 0) < 0 ? -errno : 0;
#else
    (void)pid; (void)pages; (void)count; (void)status;
    return -ENOSYS;
#endif
}

static PageRollbackResult identity_result(pid_t pid, uint64_t ticks)
{
    RuntimeMigrationMetadata metadata;

#ifdef AWAVMA_RUNTIME_TESTING
    if (test_adapter != NULL && test_adapter->force_identity_changed)
        return PAGE_ROLLBACK_IDENTITY_CHANGED;
#endif
    if (!runtime_get_migration_metadata(pid, ticks, &metadata))
        return PAGE_ROLLBACK_FAILED;
    if (!metadata.process_exists)
        return PAGE_ROLLBACK_TARGET_GONE;
    return metadata.identity_match ? PAGE_ROLLBACK_COMPLETE : PAGE_ROLLBACK_IDENTITY_CHANGED;
}

const char *page_rollback_result_name(PageRollbackResult result)
{
    static const char *names[] = {
        "PAGE_ROLLBACK_COMPLETE", "PAGE_ROLLBACK_PARTIAL", "PAGE_ROLLBACK_FAILED",
        "PAGE_ROLLBACK_UNAVAILABLE", "PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE",
        "PAGE_ROLLBACK_TARGET_GONE", "PAGE_ROLLBACK_IDENTITY_CHANGED",
        "PAGE_ROLLBACK_ATTEMPT_MISMATCH", "PAGE_ROLLBACK_QUERY_FAILED",
        "PAGE_ROLLBACK_VERIFICATION_FAILED", "PAGE_ROLLBACK_PERMISSION_DENIED",
        "PAGE_ROLLBACK_ALLOCATION_FAILED"
    };
    return result <= PAGE_ROLLBACK_ALLOCATION_FAILED ? names[result] : "PAGE_ROLLBACK_UNKNOWN";
}

PageRollbackResult page_rollback_restore(const PageRollbackRequest *request,
                                         PageRollbackSummary *summary)
{
    void **pages = NULL;
    int *nodes = NULL, *status = NULL;
    page_rollback_move_fn move;
    page_checkpoint_query_fn query;
    PageRollbackResult result;
    size_t count;

#ifdef AWAVMA_RUNTIME_TESTING
    if (test_adapter != NULL)
        test_adapter->provider_calls++;
#endif
    if (summary == NULL)
        return PAGE_ROLLBACK_FAILED;
    memset(summary, 0, sizeof(*summary));
    if (request == NULL || request->checkpoint == NULL)
        return summary->result = PAGE_ROLLBACK_UNAVAILABLE;
    if (!request->checkpoint->complete)
        return summary->result = PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE;
    if (request->checkpoint->pid != request->pid ||
        request->checkpoint->start_time_ticks != request->start_time_ticks)
        return summary->result = PAGE_ROLLBACK_IDENTITY_CHANGED;
    if (!page_checkpoint_matches_attempt(request->checkpoint, request->pid,
                                         request->start_time_ticks, request->attempt_id))
        return summary->result = PAGE_ROLLBACK_ATTEMPT_MISMATCH;
    count = request->checkpoint->requested_count;
    if (count == 0 || count > PAGE_CHECKPOINT_MAX_PAGES ||
        count > SIZE_MAX / sizeof(*pages) || count > SIZE_MAX / sizeof(*nodes) ||
        count > SIZE_MAX / sizeof(*status))
        return summary->result = PAGE_ROLLBACK_UNAVAILABLE;
    result = identity_result(request->pid, request->start_time_ticks);
    if (result != PAGE_ROLLBACK_COMPLETE)
        return summary->result = result;
    pages = calloc(count, sizeof(*pages));
    nodes = calloc(count, sizeof(*nodes));
    status = calloc(count, sizeof(*status));
    if (pages == NULL || nodes == NULL || status == NULL) {
        free(pages); free(nodes); free(status);
        return summary->result = PAGE_ROLLBACK_ALLOCATION_FAILED;
    }
    for (size_t index = 0; index < count; index++) {
        const PageCheckpointEntry *entry = &request->checkpoint->entries[index];
        if (!entry->address_valid || entry->address == NULL || !entry->original_node_known ||
            entry->original_node < 0) {
            free(pages); free(nodes); free(status);
            return summary->result = PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE;
        }
        pages[index] = entry->address;
        nodes[index] = entry->original_node;
    }
    move = request->move_fn != NULL ? request->move_fn : production_move;
    query = request->query_fn != NULL ? request->query_fn : production_query;
    summary->requested_count = count;
    summary->attempted = true;
    int move_result = move(request->context, request->pid, pages, nodes, count, status);
    if (move_result != 0) {
        result = move_result == -EPERM || move_result == -EACCES ? PAGE_ROLLBACK_PERMISSION_DENIED :
                 move_result == -ESRCH ? PAGE_ROLLBACK_TARGET_GONE : PAGE_ROLLBACK_FAILED;
        free(pages); free(nodes); free(status);
        return summary->result = result;
    }
    result = identity_result(request->pid, request->start_time_ticks);
    if (result != PAGE_ROLLBACK_COMPLETE) {
        free(pages); free(nodes); free(status);
        return summary->result = result;
    }
    memset(status, 0, count * sizeof(*status));
    int query_result = query(request->context, request->pid, pages, count, status);
    if (query_result != 0) {
        result = query_result == -EPERM || query_result == -EACCES ? PAGE_ROLLBACK_PERMISSION_DENIED :
                 query_result == -ESRCH ? PAGE_ROLLBACK_TARGET_GONE : PAGE_ROLLBACK_VERIFICATION_FAILED;
        free(pages); free(nodes); free(status);
        return summary->result = result;
    }
    for (size_t index = 0; index < count; index++) {
        if (status[index] >= 0 && status[index] == nodes[index]) {
            summary->restored_count++;
            summary->verified_count++;
        } else {
            summary->failed_count++;
        }
    }
    free(pages); free(nodes); free(status);
    return summary->result = summary->restored_count == count ? PAGE_ROLLBACK_COMPLETE :
        summary->restored_count > 0 ? PAGE_ROLLBACK_PARTIAL : PAGE_ROLLBACK_FAILED;
}
