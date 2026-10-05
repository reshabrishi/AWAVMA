#include "memory_migration_transaction.h"
#include "page_candidate_provider.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static size_t page_count_fake; static MigrationResultCode operation_result; static const char *verification;
static PageRollbackResult rollback_result; static unsigned releases, rollbacks;
static PageCheckpointResult checkpoint_result_fake; static bool source_mismatch; static unsigned operations;
static PageCheckpointResult checkpoint_fake(const PageCheckpointRequest *request, MigrationPageCheckpoint *checkpoint)
{
    if (checkpoint_result_fake != PAGE_CHECKPOINT_COMPLETE) return checkpoint_result_fake;
    checkpoint->pid = request->pid; checkpoint->start_time_ticks = request->start_time_ticks;
    snprintf(checkpoint->attempt_id, sizeof(checkpoint->attempt_id), "%s", request->attempt_id);
    checkpoint->requested_count = checkpoint->known_count = page_count_fake; checkpoint->complete = true;
    checkpoint->entries = calloc(page_count_fake, sizeof(*checkpoint->entries));
    for (size_t i = 0; i < page_count_fake; i++) { checkpoint->entries[i].original_node_known = true; checkpoint->entries[i].original_node = source_mismatch && i == 0 ? 1 : 0; }
    return PAGE_CHECKPOINT_COMPLETE;
}
static MigrationResultCode operation_fake(const MigrationRequest *request, MigrationReport *report)
{
    (void)request; operations++;
    report->pages_requested = report->pages_attempted = page_count_fake; report->result = operation_result;
    report->pages_migrated = operation_result == MIGRATION_SUCCESS ? page_count_fake : operation_result == MIGRATION_PARTIAL_SUCCESS ? 1 : 0;
    report->pages_failed = page_count_fake - report->pages_migrated; report->memory_operation_time_ms = report->pages_migrated ? 2.5 : 0.0;
    snprintf(report->verification_status, sizeof(report->verification_status), "%s", verification); return report->result;
}
static PageRollbackResult rollback_fake(const PageRollbackRequest *request, PageRollbackSummary *summary)
{ (void)request; rollbacks++; summary->result = rollback_result; summary->attempted = true; summary->requested_count = page_count_fake; summary->verified_count = rollback_result == PAGE_ROLLBACK_COMPLETE ? page_count_fake : 0; return rollback_result; }
static void release_fake(MigrationPageCheckpoint *checkpoint) { releases++; free(checkpoint->entries); memset(checkpoint, 0, sizeof(*checkpoint)); }

static void expect_invalid(MemoryMigrationTransactionRequest *request)
{
    MemoryMigrationTransactionResult result;
    assert(memory_migration_transaction_execute(request, &result));
    assert(result.outcome == MEMORY_TRANSACTION_INVALID_REQUEST);
    assert(!result.state_changed && !result.terminal_safe);
}

int main(void)
{
    MigrationRequest migration = {0};
    MemoryMigrationTransactionRequest request = {.request = &migration, .mode = MEMORY_MIGRATION_TRANSACTION_CONTROLLED_CALIBRATION, .attempt_id = "attempt",
        .expected_source_node = 0, .destination_node = 1, .expected_page_count = 4096, .require_rollback = true};
    void *pages[PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST] = {0};

    expect_invalid(&request);
    migration.phase5_decision.action = VALIDATION_ACTION_MOVE_MEMORY;
    migration.pid = 1; migration.start_time_ticks = 1; migration.start_time_ticks_available = true;
    migration.source_numa_node = 0; migration.destination_numa_node = 1;
    migration.pages = pages; migration.page_metadata_available = true;
    migration.page_addresses_authoritative = true; migration.memory_region_verified = true;
    migration.page_count = 4095; request.expected_page_count = 4095;
    /* Structurally valid counts reach checkpoint rather than failing request validation. */
    { MemoryMigrationTransactionResult result; assert(memory_migration_transaction_execute(&request, &result)); assert(result.outcome != MEMORY_TRANSACTION_INVALID_REQUEST); }
    migration.page_count = 4096; request.expected_page_count = 4096;
    { MemoryMigrationTransactionResult result; assert(memory_migration_transaction_execute(&request, &result)); assert(result.outcome != MEMORY_TRANSACTION_INVALID_REQUEST); }
    migration.page_count = 4097; request.expected_page_count = 4097; expect_invalid(&request);
    migration.page_count = 4096; request.expected_page_count = 4095; expect_invalid(&request);
    request.expected_page_count = 4096; request.destination_node = 0; expect_invalid(&request);
    request.destination_node = 1; migration.phase5_decision.action = VALIDATION_ACTION_MOVE_MEMORY;
    page_count_fake = 4096; migration.page_count = page_count_fake; request.expected_page_count = page_count_fake;
    { MemoryMigrationTransactionTestAdapter adapter = {checkpoint_fake, operation_fake, rollback_fake, release_fake};
      MemoryMigrationTransactionResult result;
      memory_migration_transaction_test_adapter_set(&adapter);
      operation_result = MIGRATION_SUCCESS; verification = "VERIFIED"; rollback_result = PAGE_ROLLBACK_COMPLETE; releases = rollbacks = 0;
      checkpoint_result_fake = PAGE_CHECKPOINT_COMPLETE; source_mismatch = false; operations = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.destination_verified && result.state_changed && result.recovery_required && result.rollback_verified && result.terminal_safe && result.migration_report.memory_operation_time_ms == 2.5 && releases == 1 && rollbacks == 1);
      operation_result = MIGRATION_PARTIAL_SUCCESS; verification = "FAILED"; rollback_result = PAGE_ROLLBACK_PARTIAL; releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.outcome == MEMORY_TRANSACTION_ROLLBACK_FAILED && result.state_changed && !result.destination_verified && !result.terminal_safe && releases == 1 && rollbacks == 1);
      operation_result = MIGRATION_SYSTEM_ERROR; verification = "FAILED"; rollback_result = PAGE_ROLLBACK_COMPLETE; releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(!result.state_changed && !result.recovery_required && result.terminal_safe && result.migration_report.memory_operation_time_ms == 0.0 && releases == 1 && rollbacks == 0);
      operation_result = MIGRATION_SUCCESS; verification = "FAILED"; rollback_result = PAGE_ROLLBACK_VERIFICATION_FAILED; releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.state_changed && !result.destination_verified && !result.terminal_safe && releases == 1 && rollbacks == 1);
      /* T05-T09: every distinct existing rollback outcome stays observable and unsafe. */
      rollback_result = PAGE_ROLLBACK_PARTIAL; verification = "VERIFIED"; operations = releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.rollback_summary.result == PAGE_ROLLBACK_PARTIAL && !result.terminal_safe && releases == 1 && rollbacks == 1);
      rollback_result = PAGE_ROLLBACK_FAILED; operations = releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.rollback_summary.result == PAGE_ROLLBACK_FAILED && !result.terminal_safe && releases == 1 && rollbacks == 1);
      rollback_result = PAGE_ROLLBACK_VERIFICATION_FAILED; operations = releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.rollback_summary.result == PAGE_ROLLBACK_VERIFICATION_FAILED && !result.terminal_safe && releases == 1 && rollbacks == 1);
      rollback_result = PAGE_ROLLBACK_ATTEMPT_MISMATCH; operations = releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.rollback_summary.result == PAGE_ROLLBACK_ATTEMPT_MISMATCH && !result.terminal_safe && releases == 1 && rollbacks == 1);
      rollback_result = PAGE_ROLLBACK_IDENTITY_CHANGED; operations = releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.rollback_summary.result == PAGE_ROLLBACK_IDENTITY_CHANGED && !result.terminal_safe && releases == 1 && rollbacks == 1);
      /* T10/T12: no acquired checkpoint means no release, operation, or rollback. */
      checkpoint_result_fake = PAGE_CHECKPOINT_INCOMPLETE; operations = releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.checkpoint_result == PAGE_CHECKPOINT_INCOMPLETE && operations == 0 && rollbacks == 0 && releases == 0 && result.migration_report.memory_operation_time_ms == 0.0);
      checkpoint_result_fake = PAGE_IDENTITY_CHANGED; operations = releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(result.checkpoint_result == PAGE_IDENTITY_CHANGED && operations == 0 && rollbacks == 0 && releases == 0);
      /* T11: complete checkpoint with conflicting physical source blocks mutation. */
      checkpoint_result_fake = PAGE_CHECKPOINT_COMPLETE; source_mismatch = true; operations = releases = rollbacks = 0;
      assert(memory_migration_transaction_execute(&request, &result)); assert(!result.source_verified && operations == 0 && rollbacks == 0 && releases == 1 && result.migration_report.memory_operation_time_ms == 0.0);
      memory_migration_transaction_test_adapter_reset(); }
    request.mode = MEMORY_MIGRATION_TRANSACTION_MODE_UNKNOWN; expect_invalid(&request);
    request.mode = MEMORY_MIGRATION_TRANSACTION_CONTROLLED_CALIBRATION; request.require_rollback = false; expect_invalid(&request);
    request.require_rollback = true; migration.page_addresses_authoritative = false; expect_invalid(&request);
    migration.page_addresses_authoritative = true; request.mode = MEMORY_MIGRATION_TRANSACTION_PRODUCTION_AUTHORIZED;
    migration.phase5_decision.action = VALIDATION_ACTION_MOVE_MEMORY; snprintf(migration.phase6_validation.final_decision, sizeof(migration.phase6_validation.final_decision), "APPROVED");
    { MemoryMigrationTransactionResult result; assert(memory_migration_transaction_execute(&request, &result)); assert(result.outcome != MEMORY_TRANSACTION_INVALID_REQUEST); }
    migration.phase5_decision.action = VALIDATION_ACTION_NO_MIGRATION; expect_invalid(&request);
    puts("memory_migration_transaction_test: PASS");
    return 0;
}
