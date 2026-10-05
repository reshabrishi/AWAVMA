#include "calibration.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    CalibrationSnapshot snapshot = {0};
    CalibrationPolicy policy;
    calibration_match_status_t status;
    char reason[CALIBRATION_REASON_MAX] = {0};

    if (argc != 2) return 2;
    calibration_policy_default(&policy);
    if (calibration_load_csv(argv[1], &policy, &snapshot, &status, reason) != 0) {
        fprintf(stderr, "%s:%s\n", calibration_match_status_name(status), reason);
        return 1;
    }
    printf("CALIBRATION_VALIDATED records=%zu\n", snapshot.count);
    calibration_snapshot_release(&snapshot);
    return 0;
}
