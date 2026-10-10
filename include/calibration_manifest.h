#ifndef AWAVMA_CALIBRATION_MANIFEST_H
#define AWAVMA_CALIBRATION_MANIFEST_H

#include "calibration.h"

#include <stdbool.h>
#include <stddef.h>

#define CALIBRATION_MANIFEST_REASON_MAX 160U

bool calibration_manifest_verify(const char *artifact_path, const char *manifest_path,
                                  const CalibrationSnapshot *snapshot,
                                  char reason[CALIBRATION_MANIFEST_REASON_MAX]);
bool calibration_manifest_verify_buffers(const char *artifact_bytes, size_t artifact_length,
                                         const char *manifest_bytes, size_t manifest_length,
                                         const char *artifact_basename,
                                         const CalibrationSnapshot *snapshot,
                                         char reason[CALIBRATION_MANIFEST_REASON_MAX]);

#endif
