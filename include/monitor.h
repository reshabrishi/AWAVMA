#ifndef AWAVMA_MONITOR_H
#define AWAVMA_MONITOR_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    const char *pattern;
    const char *threads;
    const char *memory_mb;
    const char *iterations;
    const char *duration;
} monitor_metadata_t;

typedef struct {
    pid_t pid;
    uint64_t expected_start_time_ticks;
    bool expected_start_time_available;
    unsigned interval_ms;
    const char *output_path;
    const char *thread_output_path;
    const char *log_path;
    monitor_metadata_t metadata;
    bool target_is_child;
    int *child_status;
    volatile sig_atomic_t *stop_requested;
} monitor_config_t;

/* Opaque, identity-bound state for bounded interval monitoring. */
typedef struct monitor_session monitor_session_t;

typedef enum {
    MONITOR_ACCESS_SIGNAL_UNAVAILABLE,
    MONITOR_ACCESS_SIGNAL_PROVIDER
} monitor_access_signal_source_t;

#define MONITOR_EVIDENCE_WINDOW 10U

/* Future providers must bind access evidence to an exact process identity. */
typedef struct {
    bool available;
    double value;
    pid_t pid;
    uint64_t start_time_ticks;
    uint64_t sample_elapsed_ms;
    monitor_access_signal_source_t source;
} monitor_access_evidence_t;

/* Explicitly separate real interval observations from scheduler activity. */
typedef struct {
    uint64_t completed_samples;
    uint64_t valid_interval_cpu_samples;
    bool persistent_state_ready;
    bool access_signal_available;
    monitor_access_signal_source_t access_signal_source;
    bool stability_available;
    bool classifier_confidence_available;
    bool memory_utilization_available;
    bool concurrency_available;
    bool hard_safety_evidence_complete;
} monitor_evidence_status_t;

#define MONITOR_RESULT_ERROR (-1)
#define MONITOR_RESULT_TARGET_GONE (-2)

/* Open a reusable monitoring session. The session owns a copy of config text. */
int monitor_session_open(const monitor_config_t *config, monitor_session_t **session);

/* Collect exactly one sample while retaining prior counters and thread history. */
int monitor_session_sample(monitor_session_t *session);

/* Reports only evidence actually produced by this identity-bound session. */
bool monitor_session_evidence_status(const monitor_session_t *session,
                                     monitor_evidence_status_t *status);

/* Production currently returns explicit unavailable access evidence. */
bool monitor_session_access_evidence(const monitor_session_t *session,
                                     monitor_access_evidence_t *evidence);

/* Release the bounded monitoring state and all session-owned resources. */
void monitor_session_close(monitor_session_t *session);

/* Collect one sample for an existing PID using the existing CSV formats. */
int monitor_run_pid_once(const monitor_config_t *config);

/* Monitor one existing PID until it exits or a stop signal is received. */
int monitor_run_pid(const monitor_config_t *config);

#endif
