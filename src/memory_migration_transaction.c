#include "memory_migration_transaction.h"

#include "page_candidate_provider.h"

#include <string.h>

#ifdef AWAVMA_RUNTIME_TESTING
static const MemoryMigrationTransactionTestAdapter *test_adapter;
void memory_migration_transaction_test_adapter_set(const MemoryMigrationTransactionTestAdapter *adapter) { test_adapter = adapter; }
void memory_migration_transaction_test_adapter_reset(void) { test_adapter = NULL; }
#endif

static bool request_valid(const MemoryMigrationTransactionRequest *input)
{
    const MigrationRequest *r = input == NULL ? NULL : input->request;
    if (r == NULL || input->attempt_id == NULL || input->attempt_id[0] == '\0' ||
        (input->mode != MEMORY_MIGRATION_TRANSACTION_PRODUCTION_AUTHORIZED &&
         input->mode != MEMORY_MIGRATION_TRANSACTION_CONTROLLED_CALIBRATION) ||
        (input->mode == MEMORY_MIGRATION_TRANSACTION_CONTROLLED_CALIBRATION && !input->require_rollback) ||
        (input->mode == MEMORY_MIGRATION_TRANSACTION_PRODUCTION_AUTHORIZED &&
         (r->phase5_decision.action != VALIDATION_ACTION_MOVE_MEMORY ||
          strcmp(r->phase6_validation.final_decision, "APPROVED") != 0))) return false;
    return r->pid > 0 &&
        r->start_time_ticks != 0 && r->start_time_ticks_available &&
        input->expected_source_node >= 0 && input->destination_node >= 0 &&
        input->expected_source_node != input->destination_node && input->expected_page_count > 0 &&
        input->expected_page_count == r->page_count &&
        r->page_count <= PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST &&
        r->page_count <= PAGE_CHECKPOINT_MAX_PAGES && r->pages != NULL &&
        r->page_metadata_available && r->page_addresses_authoritative && r->memory_region_verified &&
        r->source_numa_node == input->expected_source_node && r->destination_numa_node == input->destination_node;
}

bool memory_migration_transaction_execute(const MemoryMigrationTransactionRequest *input,
                                          MemoryMigrationTransactionResult *result)
{
    MigrationPageCheckpoint checkpoint = {0};
    PageCheckpointRequest capture;
    const MigrationRequest *r;
    PageRollbackRequest rollback;

    if (result == NULL) return false;
    memset(result, 0, sizeof(*result));
    result->outcome = MEMORY_TRANSACTION_INVALID_REQUEST;
    result->checkpoint_result = PAGE_ADDRESS_SET_UNAVAILABLE;
    if (!request_valid(input)) return true;
    r = input->request;
    capture = (PageCheckpointRequest){.pid = r->pid, .start_time_ticks = r->start_time_ticks,
        .attempt_id = input->attempt_id, .pages = (const void *const *)r->pages,
        .page_count = r->page_count, .authoritative_address_set = true};
    result->checkpoint_result =
#ifdef AWAVMA_RUNTIME_TESTING
        test_adapter != NULL && test_adapter->checkpoint != NULL ? test_adapter->checkpoint(&capture, &checkpoint) :
#endif
        page_checkpoint_capture(&capture, &checkpoint);
    if (result->checkpoint_result != PAGE_CHECKPOINT_COMPLETE) {
        result->outcome = MEMORY_TRANSACTION_CHECKPOINT_FAILED;
        goto done;
    }
    result->source_verified = true;
    for (size_t i = 0; i < checkpoint.requested_count; i++)
        if (!checkpoint.entries[i].original_node_known || checkpoint.entries[i].original_node != input->expected_source_node)
            result->source_verified = false;
    if (!result->source_verified) { result->outcome = MEMORY_TRANSACTION_SOURCE_MISMATCH; goto done; }
    (void)
#ifdef AWAVMA_RUNTIME_TESTING
        (test_adapter != NULL && test_adapter->operation != NULL ? test_adapter->operation(r, &result->migration_report) :
#endif
        Migration_ExecuteMemoryOperation(r, &result->migration_report)
#ifdef AWAVMA_RUNTIME_TESTING
        )
#endif
        ;
    result->state_changed = result->migration_report.pages_migrated > 0;
    result->destination_verified = result->migration_report.pages_requested == input->expected_page_count &&
        result->migration_report.pages_attempted == input->expected_page_count &&
        result->migration_report.pages_migrated == input->expected_page_count &&
        result->migration_report.pages_failed == 0 && strcmp(result->migration_report.verification_status, "VERIFIED") == 0;
    if (result->destination_verified) result->outcome = MEMORY_TRANSACTION_MIGRATION_COMPLETE;
    else if (result->migration_report.result == MIGRATION_PARTIAL_SUCCESS) result->outcome = MEMORY_TRANSACTION_MIGRATION_PARTIAL;
    else if (result->migration_report.result == MIGRATION_VERIFICATION_UNAVAILABLE || strcmp(result->migration_report.verification_status, "FAILED") == 0)
        result->outcome = MEMORY_TRANSACTION_MIGRATION_VERIFICATION_FAILED;
    else result->outcome = MEMORY_TRANSACTION_MIGRATION_FAILED;
    result->recovery_required = result->state_changed && input->require_rollback;
    if (result->recovery_required) {
        rollback = (PageRollbackRequest){.checkpoint = &checkpoint, .pid = r->pid,
            .start_time_ticks = r->start_time_ticks, .attempt_id = input->attempt_id};
        (void)
#ifdef AWAVMA_RUNTIME_TESTING
            (test_adapter != NULL && test_adapter->rollback != NULL ? test_adapter->rollback(&rollback, &result->rollback_summary) :
#endif
            page_rollback_restore(&rollback, &result->rollback_summary)
#ifdef AWAVMA_RUNTIME_TESTING
            )
#endif
            ;
        result->rollback_verified = result->rollback_summary.result == PAGE_ROLLBACK_COMPLETE &&
            result->rollback_summary.verified_count == input->expected_page_count;
        if (!result->rollback_verified) result->outcome = MEMORY_TRANSACTION_ROLLBACK_FAILED;
    }
    result->terminal_safe = !result->state_changed || !input->require_rollback || result->rollback_verified;
done:
    if (checkpoint.entries == NULL) return true;
#ifdef AWAVMA_RUNTIME_TESTING
    if (test_adapter != NULL && test_adapter->release != NULL) test_adapter->release(&checkpoint);
    else
#endif
        page_checkpoint_release(&checkpoint);
    return true;
}
