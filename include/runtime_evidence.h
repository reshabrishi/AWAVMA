#ifndef AWAVMA_RUNTIME_EVIDENCE_H
#define AWAVMA_RUNTIME_EVIDENCE_H

#include <stdint.h>

/* Convert measured Phase 3 process and thread observations into explicit runtime evidence. */
int runtime_evidence_write_classifier_input(const char *monitor_path, const char *app_id,
                                            const char *output_path);
int runtime_evidence_write_decision_input(const char *evidence_path, const char *thread_path,
                                          const char *classification_path, const char *output_path);

#endif
