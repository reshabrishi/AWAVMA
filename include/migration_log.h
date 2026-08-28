#ifndef AWAVMA_MIGRATION_LOG_H
#define AWAVMA_MIGRATION_LOG_H

#include "migration.h"

bool migration_log_init(const MigrationConfig *config);
bool migration_log_report(const MigrationRequest *request,
                          const MigrationReport *report);
bool migration_log_cleanup(void);
void migration_log_shutdown(void);

#endif
