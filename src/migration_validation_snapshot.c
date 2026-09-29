#include "migration_validation_snapshot.h"

#include <stdio.h>
#include <string.h>

bool migration_validation_snapshot_collect(pid_t pid, uint64_t start_time_ticks,
                                           const char *attempt_id,
                                           MigrationValidationSnapshot *snapshot)
{
    RuntimeMigrationMetadata metadata;

    if (snapshot == NULL || attempt_id == NULL || attempt_id[0] == '\0' ||
        !runtime_get_migration_metadata(pid, start_time_ticks, &metadata))
        return false;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->pid = pid; snapshot->start_time_ticks = start_time_ticks;
    snprintf(snapshot->attempt_id, sizeof(snapshot->attempt_id), "%s", attempt_id);
    snapshot->monotonic_ms = metadata.captured_at_ms;
    snapshot->process_exists = metadata.process_exists;
    snapshot->identity_match = metadata.identity_match;
    if (!snapshot->identity_match) return true;
    snapshot->affinity_available = metadata.affinity_available;
    snapshot->affinity = metadata.affinity;
    snapshot->process_cpu_time_available = metadata.process_cpu_time_available;
    snapshot->process_cpu_time_ticks = metadata.process_cpu_time_ticks;
    return true;
}

MigrationValidationOutcome migration_validation_compare(
    const MigrationValidationSnapshot *before, const MigrationValidationSnapshot *after,
    const cpu_set_t *requested_affinity, bool requested_affinity_available)
{
    if (before == NULL || after == NULL || before->pid != after->pid ||
        before->start_time_ticks != after->start_time_ticks ||
        strcmp(before->attempt_id, after->attempt_id) != 0)
        return MIGRATION_VALIDATION_IDENTITY_CHANGED;
    if (!after->process_exists)
        return MIGRATION_VALIDATION_TARGET_GONE;
    if (!before->identity_match || !after->identity_match)
        return MIGRATION_VALIDATION_IDENTITY_CHANGED;
    if (!before->affinity_available || !after->affinity_available ||
        after->monotonic_ms < before->monotonic_ms)
        return MIGRATION_VALIDATION_INSUFFICIENT_DATA;
    if (requested_affinity_available &&
        (requested_affinity == NULL || !CPU_EQUAL(&after->affinity, requested_affinity)))
        return MIGRATION_VALIDATION_PLACEMENT_NOT_APPLIED;
    if (!before->process_cpu_time_available || !after->process_cpu_time_available)
        return MIGRATION_VALIDATION_INSUFFICIENT_DATA;
    if (after->process_cpu_time_ticks > before->process_cpu_time_ticks)
        return MIGRATION_VALIDATION_STRUCTURALLY_VALID_PROGRESS;
    return MIGRATION_VALIDATION_NO_PROGRESS_INCONCLUSIVE;
}
