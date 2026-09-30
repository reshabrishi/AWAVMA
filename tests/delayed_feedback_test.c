#include "feedback_types.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    FeedbackEvent event = {0};

    snprintf(event.app_id, sizeof(event.app_id), "controlled-app");
    snprintf(event.migration_id, sizeof(event.migration_id), "attempt-1");
    snprintf(event.phase5_migration_id, sizeof(event.phase5_migration_id), "phase-attempt-1");
    snprintf(event.phase6_migration_id, sizeof(event.phase6_migration_id), "phase-attempt-1");
    event.event_kind = FEEDBACK_EVENT_MIGRATION_OBSERVATION;
    event.before_samples = 5;
    event.after_samples = 5;
    event.sample_counts_available = true;
    for (size_t metric = 0; metric < FEEDBACK_METRIC_COUNT; metric++) {
        event.before_metrics[metric] = 10.0;
        event.after_metrics[metric] = 9.0;
        event.metric_available[metric] = true;
    }
    assert(event.event_kind == FEEDBACK_EVENT_MIGRATION_OBSERVATION);
    assert(event.before_samples == event.after_samples);
    assert(strcmp(event.phase5_migration_id, event.phase6_migration_id) == 0);
    assert(event.metric_available[FEEDBACK_METRIC_PAGE_FAULTS]);
    return 0;
}
