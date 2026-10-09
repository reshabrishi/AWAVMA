#include "p5_thread_activity_calibration_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    P5ThreadActivityCalibration calibration;
    P5ThreadActivityRawSample *samples = NULL;
    size_t sample_count = 0;
    char reason[P5_THREAD_ACTIVITY_REASON_MAX] = {0};
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("usage: %s ROOT CALIBRATION_ID\n", argv[0]);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s ROOT CALIBRATION_ID\n", argv[0]);
        return 2;
    }
    if (p5_thread_activity_calibration_load_artifacts(argv[1], argv[2], &calibration, &samples,
                                                       &sample_count, reason, sizeof(reason)) != 0) {
        fprintf(stderr, "status=INVALID reason=%s\n", reason);
        free(samples);
        return 1;
    }
    printf("status=VALID calibration_id=%s samples=%zu calibration_status=%s\n", argv[2], sample_count,
           p5_thread_activity_calibration_status_name(calibration.status));
    free(samples);
    return 0;
}
