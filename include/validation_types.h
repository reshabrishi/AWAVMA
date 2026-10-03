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
    GATE_INVALID,
    GATE_NOT_APPLICABLE
} GateStatus;

typedef enum {
    DECISION_EVIDENCE_UNAVAILABLE,
    DECISION_EVIDENCE_UTILITY_POLICY,
    DECISION_EVIDENCE_EMPIRICAL_GAIN_COST
} DecisionEvidenceModel;

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
    /* Runtime-owned, candidate-specific MOVE_THREAD evidence. */
    bool thread_confidence_evidence_valid;
    bool thread_safety_evidence_valid;
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
    DecisionEvidenceModel evidence_model;
    /* Exact Phase 5 decision-engine evidence; no utility is recalculated here. */
    bool phase5_evidence_available;
    char phase5_timestamp[32];
    uint64_t phase5_runtime_generation;
    bool phase5_runtime_generation_available;
    char phase5_evidence_provenance[48];
    char phase5_classification[16];
    char phase5_decision_status[64];
    double phase5_classification_score;
    bool phase5_classification_score_available;
    double phase5_factors[10];
    bool phase5_factor_available[10];
    double phase5_memory_score_raw;
    double phase5_thread_score_raw;
    double phase5_memory_bias;
    double phase5_thread_bias;
    double phase5_memory_score_final;
    double phase5_thread_score_final;
    bool phase5_utility_available;
    /* Dimensionless final-utility difference, not measured performance gain. */
    double phase5_decision_margin;
    bool phase5_decision_margin_available;
    double phase5_epsilon;
    bool phase5_epsilon_available;
    unsigned phase5_weight_version;
    unsigned phase5_bias_version;
    bool phase5_versions_available;
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
    int source_node;
    int destination_node;
    double confidence_score;
    double roi_score;
    double safety_score;
    double validation_score;
    GateStatus confidence_status;
    GateStatus roi_status;
    GateStatus safety_status;
    /* Page controls are meaningful only for MOVE_MEMORY; MOVE_THREAD records N/A. */
    char page_locked_field[8];
    char memory_pinned_field[8];
    char validation_status[64];
    char final_decision[32];
} ValidationResult;

#endif
