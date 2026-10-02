#include "migration_cost.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char *text, double *value)
{
    char *end = NULL;
    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' && isfinite(*value);
}

bool migration_cost_artifact_load(const char *path, int source_node, int target_node,
                                  MigrationCostArtifact *artifact)
{
    char header[256], row[512], *fields[7], *save = NULL, *end = NULL;
    FILE *file;
    size_t count = 0;
    long parsed_source, parsed_target;
    double cost;

    if (artifact != NULL) memset(artifact, 0, sizeof(*artifact));
    if (path == NULL || source_node < 0 || target_node < 0 || source_node == target_node)
        return false;
    file = fopen(path, "r");
    if (file == NULL || fgets(header, sizeof(header), file) == NULL ||
        fgets(row, sizeof(row), file) == NULL || fgetc(file) != EOF) {
        if (file != NULL) fclose(file);
        return false;
    }
    fclose(file);
    if (strcmp(header, "schema_version,source_node,target_node,migration_cost_seconds,validation_status,provenance\n") != 0)
        return false;
    row[strcspn(row, "\r\n")] = '\0';
    for (char *token = strtok_r(row, ",", &save); token != NULL && count < 6;
         token = strtok_r(NULL, ",", &save)) fields[count++] = token;
    if (count != 6 || strcmp(fields[0], "1") != 0 || strcmp(fields[4], "PASS") != 0 ||
        fields[5][0] == '\0' ||
        (parsed_source = strtol(fields[1], &end, 10), end == fields[1] || *end != '\0') ||
        (parsed_target = strtol(fields[2], &end, 10), end == fields[2] || *end != '\0') ||
        parsed_source != source_node || parsed_target != target_node ||
        !number(fields[3], &cost) || cost < 0.0)
        return false;
    if (artifact != NULL) {
        artifact->available = true;
        artifact->source_node = source_node;
        artifact->target_node = target_node;
        artifact->migration_cost_seconds = cost;
        snprintf(artifact->provenance, sizeof(artifact->provenance), "%s", fields[5]);
    }
    return true;
}

bool migration_cost_roi_time_equivalent(double horizon_seconds, double gain_percent,
                                        const MigrationCostArtifact *artifact,
                                        MigrationCostROI *roi)
{
    double gain;
    if (roi == NULL) return false;
    memset(roi, 0, sizeof(*roi));
    if (artifact == NULL || !artifact->available || !isfinite(horizon_seconds) || horizon_seconds <= 0.0 ||
        !isfinite(gain_percent) || gain_percent <= 0.0 || !isfinite(artifact->migration_cost_seconds) ||
        artifact->migration_cost_seconds <= 0.0)
        return false;
    gain = gain_percent / 100.0;
    roi->available = true;
    roi->horizon_seconds = horizon_seconds;
    roi->gain_fraction = gain;
    roi->migration_cost_seconds = artifact->migration_cost_seconds;
    roi->saved_time_seconds = horizon_seconds * gain;
    roi->net_time_seconds = roi->saved_time_seconds - artifact->migration_cost_seconds;
    roi->benefit_cost_ratio = roi->saved_time_seconds / artifact->migration_cost_seconds;
    roi->break_even_horizon_seconds = artifact->migration_cost_seconds / gain;
    return true;
}
