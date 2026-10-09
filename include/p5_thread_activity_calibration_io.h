#ifndef AWAVMA_P5_THREAD_ACTIVITY_CALIBRATION_IO_H
#define AWAVMA_P5_THREAD_ACTIVITY_CALIBRATION_IO_H

#include "p5_thread_activity_calibration.h"

#define P5_THREAD_ACTIVITY_ARTIFACT_FORMAT_VERSION 1U
#define P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME "p5_thread_activity_calibration"

bool p5_thread_activity_calibration_id_valid(const char *calibration_id);
int p5_thread_activity_calibration_write_artifacts(const char *root, const P5ThreadActivityContext *context,
                                                    const char *calibration_id, const P5ThreadActivityRawSample *samples,
                                                    size_t sample_count, const P5ThreadActivityCalibration *calibration,
                                                    char *reason, size_t reason_size);
int p5_thread_activity_calibration_load_artifacts(const char *root, const char *calibration_id,
                                                   P5ThreadActivityCalibration *calibration,
                                                   P5ThreadActivityRawSample **samples, size_t *sample_count,
                                                   char *reason, size_t reason_size);

#endif
