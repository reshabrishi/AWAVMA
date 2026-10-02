#ifndef AWAVMA_MIGRATION_COST_H
#define AWAVMA_MIGRATION_COST_H

#include <stdbool.h>

typedef struct {
    bool available;
    int source_node;
    int target_node;
    double migration_cost_seconds;
    char provenance[128];
} MigrationCostArtifact;

typedef struct {
    bool available;
    double horizon_seconds;
    double gain_fraction;
    double migration_cost_seconds;
    double saved_time_seconds;
    double net_time_seconds;
    double benefit_cost_ratio;
    double break_even_horizon_seconds;
} MigrationCostROI;

/* Loads one strict, directed route artifact. It never substitutes a reverse route. */
bool migration_cost_artifact_load(const char *path, int source_node, int target_node,
                                  MigrationCostArtifact *artifact);
/* H is active workload time. saved(H)=H*g; net(H)=H*g-C; break_even=C/g. */
bool migration_cost_roi_time_equivalent(double horizon_seconds, double gain_percent,
                                        const MigrationCostArtifact *artifact,
                                        MigrationCostROI *roi);

#endif
