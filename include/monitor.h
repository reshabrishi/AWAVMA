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

#define MONITOR_RESULT_ERROR (-1)
#define MONITOR_RESULT_TARGET_GONE (-2)

typedef struct monitor_session monitor_session_t;

/* An opaque persistent state for repeated samples of one exact process identity. */
monitor_session_t *monitor_session_create(const monitor_config_t *config);
int monitor_session_sample(monitor_session_t *session);
void monitor_session_destroy(monitor_session_t *session);

/* Collect one sample for an existing PID using the existing CSV formats. */
int monitor_run_pid_once(const monitor_config_t *config);

/* Monitor one existing PID until it exits or a stop signal is received. */
int monitor_run_pid(const monitor_config_t *config);

#endif
