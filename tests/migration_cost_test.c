#include "migration_cost.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    const char *path = "/tmp/awavma-migration-cost.csv";
    MigrationCostArtifact artifact;
    MigrationCostROI roi;
    FILE *file = fopen(path, "w");

    assert(file != NULL);
    fputs("schema_version,source_node,target_node,migration_cost_seconds,validation_status,provenance\n"
          "1,1,0,0.250000000,PASS,measured_controlled_route\n", file);
    assert(fclose(file) == 0);
    assert(migration_cost_artifact_load(path, 1, 0, &artifact));
    assert(!migration_cost_artifact_load(path, 0, 1, &artifact));
    assert(migration_cost_artifact_load(path, 1, 0, &artifact));
    artifact.migration_cost_seconds = 0.5;
    assert(migration_cost_roi_time_equivalent(10.0, 20.0, &artifact, &roi));
    assert(fabs(roi.saved_time_seconds - 2.0) < 1e-9);
    assert(fabs(roi.net_time_seconds - 1.5) < 1e-9);
    assert(fabs(roi.benefit_cost_ratio - 4.0) < 1e-9);
    artifact.migration_cost_seconds = 2.0;
    assert(migration_cost_roi_time_equivalent(10.0, 20.0, &artifact, &roi));
    assert(fabs(roi.net_time_seconds) < 1e-9);
    puts("migration_cost_test: PASS");
    return 0;
}
