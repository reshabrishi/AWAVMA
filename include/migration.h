#ifndef AWAVMA_MIGRATION_H
#define AWAVMA_MIGRATION_H

#include "migration_types.h"

#include <stdbool.h>

typedef struct {
    const char *results_path;
    const char *history_path;
    const char *log_path;
    const char *state_path;
    size_t history_max_records;
    double history_max_days;
    double history_decay_lambda;
    size_t cleanup_interval;
    unsigned cooldown_ms;
    unsigned lock_timeout_ms;
    bool verification_enabled;
} MigrationConfig;

const char *MigrationResultName(MigrationResultCode result);
const char *MigrationActionName(ValidationAction action);
bool Migration_Init(const MigrationConfig *config);
MigrationResultCode Migration_Execute(const MigrationRequest *request,
                                      MigrationReport *report);
bool Migration_AcquireExecutionLock(const MigrationRequest *request);
void Migration_ReleaseExecutionLock(const MigrationRequest *request);
bool Migration_CleanupHistory(void);
void Migration_Shutdown(void);

#endif
