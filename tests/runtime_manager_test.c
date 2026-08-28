#define _POSIX_C_SOURCE 200809L

#include "application_manager.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        pause();
        _exit(0);
    }

    application_manager_config_t config = {
        .scan_interval_ms = 10,
        .monitor_interval_ms = 10,
        .idle_timeout_seconds = 1,
        .worker_count = 2,
        .max_pending_jobs = 2,
        .max_applications = 32,
        .monitor_path = "bin/monitor",
        .include_root_processes = true,
        .results_dir = "results/runtime_test",
        .log_path = "logs/application_manager.log",
        .worker_log_path = "logs/worker_pool.log",
        .dry_run = true,
        .once = false,
        .duration_ms = 0,
        .stop_requested = NULL
    };
    application_manager_t *manager = application_manager_create();
    assert(manager != NULL);
    assert(application_manager_init(manager, &config) == 0);
    assert(application_manager_discover_once(manager) == 0);
    application_identity_t snapshot[32];
    size_t count = application_manager_snapshot(manager, snapshot, 32);
    assert(count > 0);
    pid_t tracked_pid = snapshot[0].pid;
    uint64_t tracked_start = snapshot[0].start_time_ticks;
    assert(tracked_start != 0);
    assert(application_manager_discover_once(manager) == 0);
    application_identity_t tracked_record;
    assert(application_manager_find(manager, tracked_pid, tracked_start, &tracked_record));
    assert(tracked_record.start_time_ticks == tracked_start);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    application_manager_shutdown(manager);
    application_manager_destroy(manager);
    puts("runtime manager C tests: PASS");
    return 0;
}
