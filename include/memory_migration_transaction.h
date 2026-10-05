#ifndef AWAVMA_MEMORY_MIGRATION_TRANSACTION_H
#define AWAVMA_MEMORY_MIGRATION_TRANSACTION_H

#include "migration.h"
#include "page_checkpoint.h"
#include "page_rollback.h"

typedef enum {
    MEMORY_TRANSACTION_INVALID_REQUEST,
    MEMORY_TRANSACTION_CHECKPOINT_FAILED,
    MEMORY_TRANSACTION_SOURCE_MISMATCH,
    MEMORY_TRANSACTION_MIGRATION_COMPLETE,
    MEMORY_TRANSACTION_MIGRATION_PARTIAL,
    MEMORY_TRANSACTION_MIGRATION_FAILED,
    MEMORY_TRANSACTION_MIGRATION_VERIFICATION_FAILED,
    MEMORY_TRANSACTION_ROLLBACK_FAILED
} MemoryMigrationTransactionOutcome;
typedef enum {
    MEMORY_MIGRATION_TRANSACTION_MODE_UNKNOWN = 0,
    MEMORY_MIGRATION_TRANSACTION_PRODUCTION_AUTHORIZED,
    MEMORY_MIGRATION_TRANSACTION_CONTROLLED_CALIBRATION
} MemoryMigrationTransactionMode;

typedef struct {
    const MigrationRequest *request;
    MemoryMigrationTransactionMode mode;
    const char *attempt_id;
    int expected_source_node;
    int destination_node;
    size_t expected_page_count;
    bool require_rollback;
} MemoryMigrationTransactionRequest;

typedef struct {
    MemoryMigrationTransactionOutcome outcome;
    PageCheckpointResult checkpoint_result;
    bool source_verified;
    MigrationReport migration_report;
    bool destination_verified;
    PageRollbackSummary rollback_summary;
    bool rollback_verified;
    bool state_changed;
    bool recovery_required;
    bool terminal_safe;
} MemoryMigrationTransactionResult;

bool memory_migration_transaction_execute(const MemoryMigrationTransactionRequest *request,
                                          MemoryMigrationTransactionResult *result);
#ifdef AWAVMA_RUNTIME_TESTING
typedef PageCheckpointResult (*memory_transaction_checkpoint_fn)(const PageCheckpointRequest *, MigrationPageCheckpoint *);
typedef MigrationResultCode (*memory_transaction_operation_fn)(const MigrationRequest *, MigrationReport *);
typedef PageRollbackResult (*memory_transaction_rollback_fn)(const PageRollbackRequest *, PageRollbackSummary *);
typedef void (*memory_transaction_release_fn)(MigrationPageCheckpoint *);
typedef struct { memory_transaction_checkpoint_fn checkpoint; memory_transaction_operation_fn operation;
    memory_transaction_rollback_fn rollback; memory_transaction_release_fn release; } MemoryMigrationTransactionTestAdapter;
void memory_migration_transaction_test_adapter_set(const MemoryMigrationTransactionTestAdapter *adapter);
void memory_migration_transaction_test_adapter_reset(void);
#endif
#endif
