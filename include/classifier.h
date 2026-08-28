#ifndef AWAVMA_CLASSIFIER_H
#define AWAVMA_CLASSIFIER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CLASS_COLD,
    CLASS_MODERATE,
    CLASS_HOT,
    CLASS_NONE,
    CLASS_UNAVAILABLE
} page_class_t;

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *access_column;
    const char *entity_column;
    size_t window_size;
    double lambda;
    double hot_threshold;
    double moderate_threshold;
    double hysteresis;
    bool require_access;
} classifier_config_t;

typedef struct {
    size_t rows;
    size_t classified_rows;
    size_t unavailable_rows;
    size_t hot_rows;
    size_t moderate_rows;
    size_t cold_rows;
    double elapsed_seconds;
} classifier_summary_t;

const char *classifier_class_name(page_class_t classification);
void classifier_config_default(classifier_config_t *config);
int classifier_run(const classifier_config_t *config, classifier_summary_t *summary);

#endif
