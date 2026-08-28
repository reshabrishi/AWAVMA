#ifndef AWAVMA_VALIDATION_TYPES_H
#define AWAVMA_VALIDATION_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VALIDATION_ACTION_NO_MIGRATION,
    VALIDATION_ACTION_MOVE_MEMORY,
    VALIDATION_ACTION_MOVE_THREAD,
    VALIDATION_ACTION_INSUFFICIENT
} ValidationAction;

typedef enum {
    GATE_PASS,
    GATE_FAIL,
    GATE_INVALID
} GateStatus;

typedef struct {
    bool available;
    double stability;
    uint64_t sample_count;
    double cpu_utilization;
    double memory_utilization;
    double concurrency_score;
    bool page_locked;
    bool memory_pinned;
    bool cooldown_active;
    bool thread_locked;
    bool migration_in_progress;
    bool max_migrations_reached;
    bool hard_constraints_available;
} MonitorData;

typedef struct {
    bool available;
    char classification[16];
    double confidence;
} ClassifierData;

typedef struct {
    ValidationAction action;
    char action_text[40];
    char status_text[64];
    char migration_id[128];
    char app_id[128];
    long pid;
    char entity_id[128];
    int source_node;
    int destination_node;
    bool nodes_available;
    double predicted_gain;
    double estimated_cost;
    bool gain_available;
    bool cost_available;
} DecisionData;

typedef struct {
    GateStatus status;
    double score;
    char reason[64];
} GateResult;

typedef struct {
    char timestamp[32];
    char migration_id[128];
    char app_id[128];
    long pid;
    char entity_id[128];
    ValidationAction action;
    double confidence_score;
    double roi_score;
    double safety_score;
    double validation_score;
    GateStatus confidence_status;
    GateStatus roi_status;
    GateStatus safety_status;
    char validation_status[64];
    char final_decision[32];
} ValidationResult;

#endif
