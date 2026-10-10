#define _POSIX_C_SOURCE 200809L

#include "page_checkpoint.h"

#include "migration_target_provider.h"
#include "runtime_migration_metadata.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int query_pages(void *context, pid_t pid, void **pages, size_t page_count, int *status)
{
    (void)context;
#ifdef SYS_move_pages
    return syscall(SYS_move_pages, pid, page_count, pages, NULL, status, 0) < 0 ? -errno : 0;
#else
    (void)pid;
    (void)pages;
    (void)page_count;
    (void)status;
    return -ENOSYS;
#endif
}

static PageCheckpointResult identity_result(pid_t pid, uint64_t start_time_ticks)
{
    RuntimeMigrationMetadata metadata;

    if (!runtime_get_migration_metadata(pid, start_time_ticks, &metadata))
        return PAGE_QUERY_FAILED;
    if (!metadata.process_exists)
        return PAGE_TARGET_GONE;
    return metadata.identity_match ? PAGE_CHECKPOINT_COMPLETE : PAGE_IDENTITY_CHANGED;
}

static bool node_is_online(const MigrationTargetTopology *topology, int node)
{
    if (node < 0 || node >= (int)MIGRATION_TARGET_MAX_NODES || !topology->node_present[node])
        return false;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (topology->cpu_node[cpu] == node && CPU_ISSET(cpu, &topology->online_cpus))
            return true;
    return false;
}

const char *page_checkpoint_result_name(PageCheckpointResult result)
{
    static const char *names[] = {
        "PAGE_CHECKPOINT_COMPLETE", "PAGE_ADDRESS_SET_UNAVAILABLE", "PAGE_ADDRESS_INVALID",
        "PAGE_ADDRESS_DUPLICATE", "PAGE_QUERY_PERMISSION_DENIED", "PAGE_QUERY_PARTIAL",
        "PAGE_QUERY_FAILED", "PAGE_CHECKPOINT_INCOMPLETE", "PAGE_TARGET_GONE",
        "PAGE_IDENTITY_CHANGED", "PAGE_CHECKPOINT_ALLOCATION_FAILED",
        "PAGE_CHECKPOINT_TOO_LARGE", "PAGE_CHECKPOINT_STALE"
    };

    return result <= PAGE_CHECKPOINT_STALE ? names[result] : "PAGE_CHECKPOINT_UNKNOWN";
}

void page_checkpoint_release(MigrationPageCheckpoint *checkpoint)
{
    if (checkpoint == NULL)
        return;
    free(checkpoint->entries);
    memset(checkpoint, 0, sizeof(*checkpoint));
}

bool page_checkpoint_recovery_evidence(const MigrationPageCheckpoint *checkpoint,
                                       bool rollback_provider_retained,
                                       int source_numa_node, int target_numa_node,
                                       MemoryRecoveryEvidence *evidence)
{
    if (checkpoint == NULL || evidence == NULL || checkpoint->pid <= 0 ||
        checkpoint->start_time_ticks == 0 || checkpoint->attempt_id[0] == '\0' ||
        !checkpoint->complete || checkpoint->requested_count == 0 ||
        checkpoint->known_count != checkpoint->requested_count || checkpoint->unknown_count != 0 ||
        source_numa_node < 0 || target_numa_node < 0 || source_numa_node == target_numa_node)
        return false;
    for (size_t index = 0; index < checkpoint->requested_count; index++)
        if (checkpoint->entries == NULL || !checkpoint->entries[index].original_node_known ||
            checkpoint->entries[index].original_node != source_numa_node)
            return false;
    memset(evidence, 0, sizeof(*evidence));
    evidence->pid = checkpoint->pid;
    evidence->start_time_ticks = checkpoint->start_time_ticks;
    snprintf(evidence->attempt_id, sizeof(evidence->attempt_id), "%s", checkpoint->attempt_id);
    evidence->candidate_count = checkpoint->requested_count;
    evidence->checkpoint_complete = true;
    evidence->original_placement_known = true;
    evidence->rollback_provider_retained = rollback_provider_retained;
    evidence->source_numa_node = source_numa_node;
    evidence->target_numa_node = target_numa_node;
    return true;
}

bool page_checkpoint_matches_attempt(const MigrationPageCheckpoint *checkpoint, pid_t pid,
                                     uint64_t start_time_ticks, const char *attempt_id)
{
    return checkpoint != NULL && checkpoint->complete && checkpoint->pid == pid &&
           checkpoint->start_time_ticks == start_time_ticks && attempt_id != NULL &&
           strcmp(checkpoint->attempt_id, attempt_id) == 0;
}

PageCheckpointResult page_checkpoint_capture(const PageCheckpointRequest *request,
                                              MigrationPageCheckpoint *checkpoint)
{
    MigrationTargetTopology topology;
    PageCheckpointResult result;
    page_checkpoint_query_fn query;
    int *status = NULL;
    void **pages = NULL;
    long page_size;

    if (checkpoint == NULL)
        return PAGE_QUERY_FAILED;
    page_checkpoint_release(checkpoint);
    if (request == NULL || !request->authoritative_address_set || request->pages == NULL ||
        request->page_count == 0)
        return PAGE_ADDRESS_SET_UNAVAILABLE;
    if (request->pid <= 0 || request->start_time_ticks == 0 || request->attempt_id == NULL ||
        request->attempt_id[0] == '\0')
        return PAGE_IDENTITY_CHANGED;
    if (request->page_count > PAGE_CHECKPOINT_MAX_PAGES)
        return PAGE_CHECKPOINT_TOO_LARGE;
    if (request->page_count > SIZE_MAX / sizeof(*checkpoint->entries) ||
        request->page_count > SIZE_MAX / sizeof(*status) ||
        request->page_count > SIZE_MAX / sizeof(*pages))
        return PAGE_CHECKPOINT_TOO_LARGE;
    result = identity_result(request->pid, request->start_time_ticks);
    if (result != PAGE_CHECKPOINT_COMPLETE)
        return result;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return PAGE_QUERY_FAILED;
    checkpoint->entries = calloc(request->page_count, sizeof(*checkpoint->entries));
    status = calloc(request->page_count, sizeof(*status));
    pages = calloc(request->page_count, sizeof(*pages));
    if (checkpoint->entries == NULL || status == NULL || pages == NULL) {
        free(status);
        free(pages);
        page_checkpoint_release(checkpoint);
        return PAGE_CHECKPOINT_ALLOCATION_FAILED;
    }
    checkpoint->pid = request->pid;
    checkpoint->start_time_ticks = request->start_time_ticks;
    snprintf(checkpoint->attempt_id, sizeof(checkpoint->attempt_id), "%s", request->attempt_id);
    checkpoint->system_page_size = (size_t)page_size;
    checkpoint->requested_count = request->page_count;
    clock_gettime(CLOCK_MONOTONIC, &checkpoint->captured_at);
    for (size_t index = 0; index < request->page_count; index++) {
        uintptr_t address = (uintptr_t)request->pages[index];

        if (address == 0 || address % (uintptr_t)page_size != 0) {
            page_checkpoint_release(checkpoint);
            free(status);
            free(pages);
            return PAGE_ADDRESS_INVALID;
        }
        for (size_t previous = 0; previous < index; previous++)
            if (request->pages[previous] == request->pages[index]) {
                page_checkpoint_release(checkpoint);
                free(status);
                free(pages);
                return PAGE_ADDRESS_DUPLICATE;
            }
        checkpoint->entries[index].address = (void *)address;
        checkpoint->entries[index].address_valid = true;
        checkpoint->entries[index].original_node = -1;
        pages[index] = (void *)address;
    }
    query = request->query_fn != NULL ? request->query_fn : query_pages;
    int query_result = query(request->query_context, request->pid, pages, request->page_count, status);
    free(pages);
    if (query_result != 0) {
        free(status);
        result = query_result == -EPERM || query_result == -EACCES ? PAGE_QUERY_PERMISSION_DENIED :
                 query_result == -ESRCH ? PAGE_TARGET_GONE : PAGE_QUERY_FAILED;
        page_checkpoint_release(checkpoint);
        return result;
    }
    result = identity_result(request->pid, request->start_time_ticks);
    if (result != PAGE_CHECKPOINT_COMPLETE) {
        free(status);
        page_checkpoint_release(checkpoint);
        return result;
    }
    if (!migration_target_topology_read(&topology)) {
        free(status);
        page_checkpoint_release(checkpoint);
        return PAGE_QUERY_FAILED;
    }
    for (size_t index = 0; index < request->page_count; index++) {
        checkpoint->entries[index].query_status = status[index];
        if (status[index] >= 0 && node_is_online(&topology, status[index])) {
            checkpoint->entries[index].original_node_known = true;
            checkpoint->entries[index].original_node = status[index];
            checkpoint->known_count++;
        } else {
            checkpoint->unknown_count++;
        }
    }
    free(status);
    checkpoint->complete = checkpoint->known_count == checkpoint->requested_count;
    checkpoint->result = checkpoint->complete ? PAGE_CHECKPOINT_COMPLETE : PAGE_CHECKPOINT_INCOMPLETE;
    return checkpoint->result;
}
