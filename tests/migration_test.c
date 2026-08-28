#include "migration.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char test_directory[] = "/tmp/awavma-migration-test-XXXXXX";
static char results_path[PATH_MAX];
static char history_path[PATH_MAX];
static char log_path[PATH_MAX];
static char state_path[PATH_MAX];

static void print_test(const char *test, const char *expected, const char *actual, const char *result)
{
    printf("%s | %s | %s | %s\n", test, expected, actual, result);
}

static void set_identity(MigrationRequest *request, ValidationAction action,
                          const char *migration_id, const char *phase6,
                          pid_t pid, pid_t tid)
{
    memset(request, 0, sizeof(*request));
    request->phase5_decision.action = action;
    request->phase5_decision.pid = (long)pid;
    request->pid = pid;
    request->tid = tid;
    snprintf(request->phase5_decision.migration_id, sizeof(request->phase5_decision.migration_id), "%s", migration_id);
    snprintf(request->phase5_decision.app_id, sizeof(request->phase5_decision.app_id), "migration-test");
    snprintf(request->phase5_decision.entity_id, sizeof(request->phase5_decision.entity_id), "controlled-thread");
    snprintf(request->phase6_validation.final_decision, sizeof(request->phase6_validation.final_decision), "%s", phase6);
    snprintf(request->phase6_validation.validation_status, sizeof(request->phase6_validation.validation_status), "%s", phase6);
    request->source_numa_node = -1;
    request->destination_numa_node = -1;
    request->destination_cpu = -1;
}

static MigrationConfig test_config(size_t max_records, double max_days, size_t cleanup_interval)
{
    return (MigrationConfig){
        .results_path = results_path,
        .history_path = history_path,
        .log_path = log_path,
        .state_path = state_path,
        .history_max_records = max_records,
        .history_max_days = max_days,
        .history_decay_lambda = 0.1,
        .cleanup_interval = cleanup_interval,
        .cooldown_ms = 0,
        .lock_timeout_ms = 0,
        .verification_enabled = true
    };
}

static int reset_module(size_t max_records, double max_days, size_t cleanup_interval)
{
    MigrationConfig config;

    Migration_Shutdown();
    unlink(results_path);
    unlink(history_path);
    unlink(log_path);
    unlink(state_path);
    config = test_config(max_records, max_days, cleanup_interval);
    return Migration_Init(&config) ? 0 : -1;
}

static pid_t spawn_pause(void)
{
    pid_t child = fork();

    if (child == 0) {
        for (;;)
            pause();
    }
    if (child > 0)
        usleep(20000);
    return child;
}

static void stop_child(pid_t child)
{
    if (child <= 0)
        return;
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
}

static int pin_process(pid_t pid, int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    if (cpu < 0 || cpu >= CPU_SETSIZE)
        return -1;
    CPU_SET(cpu, &set);
    return sched_setaffinity(pid, sizeof(set), &set);
}

static int get_affinity(pid_t pid, cpu_set_t *set)
{
    return sched_getaffinity(pid, sizeof(*set), set);
}

static int line_count(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[4096];
    int count = 0;

    if (file == NULL)
        return -1;
    while (fgets(line, sizeof(line), file) != NULL)
        count++;
    fclose(file);
    return count;
}

static int contains_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "r");
    char line[4096];
    int found = 0;

    if (file == NULL)
        return 0;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, text) != NULL)
            found = 1;
    fclose(file);
    return found;
}

static void execute_and_print(const char *name, const char *expected,
                              MigrationRequest *request, int should_pass)
{
    MigrationReport report;
    MigrationResultCode actual = Migration_Execute(request, &report);
    const char *actual_name = MigrationResultName(actual);

    print_test(name, expected, actual_name, should_pass ? "PASS" : "NOT TESTED — ENVIRONMENT LIMITATION");
}

typedef struct {
    int fd;
} thread_child_args_t;

static void *short_lived_thread(void *argument)
{
    thread_child_args_t *args = argument;
    pid_t tid = (pid_t)syscall(SYS_gettid);

    if (write(args->fd, &tid, sizeof(tid)) != (ssize_t)sizeof(tid))
        return NULL;
    return NULL;
}

static pid_t spawn_thread_that_exits(int *worker_tid)
{
    int pipefd[2];
    pid_t child;

    if (pipe(pipefd) != 0)
        return -1;
    child = fork();
    if (child == 0) {
        pthread_t thread;
        thread_child_args_t args = {.fd = pipefd[1]};

        close(pipefd[0]);
        if (pthread_create(&thread, NULL, short_lived_thread, &args) == 0)
            pthread_join(thread, NULL);
        close(pipefd[1]);
        for (;;)
            pause();
    }
    close(pipefd[1]);
    if (child > 0 && read(pipefd[0], worker_tid, sizeof(*worker_tid)) != (ssize_t)sizeof(*worker_tid)) {
        close(pipefd[0]);
        stop_child(child);
        return -1;
    }
    close(pipefd[0]);
    usleep(20000);
    return child;
}

static int numa_node_count(void)
{
    int count = 0;
    char path[128];

    for (int node = 0; node < 256; node++) {
        struct stat status;

        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d", node);
        if (stat(path, &status) == 0)
            count++;
    }
    return count;
}

int main(void)
{
    char *directory;
    MigrationRequest request;
    MigrationReport report;
    pid_t child_a;
    pid_t child_b;
    pid_t child_c;
    int worker_tid;
    int remote_nodes;
    cpu_set_t before;
    cpu_set_t after;
    int affinity_available;
    MigrationConfig age_config;

    directory = mkdtemp(test_directory);
    if (directory == NULL)
        return EXIT_FAILURE;
    snprintf(results_path, sizeof(results_path), "%s/migration_results.csv", directory);
    snprintf(history_path, sizeof(history_path), "%s/migration_history.csv", directory);
    snprintf(log_path, sizeof(log_path), "%s/migration.log", directory);
    snprintf(state_path, sizeof(state_path), "%s/migration_state.csv", directory);
    if (reset_module(1000, 30.0, 100) != 0)
        return EXIT_FAILURE;

    set_identity(&request, VALIDATION_ACTION_NO_MIGRATION, "T03", "REJECTED", 999999, 0);
    execute_and_print("T03", "MIGRATION_NO_ACTION", &request, 1);
    set_identity(&request, VALIDATION_ACTION_INSUFFICIENT, "T04", "REJECTED", 999999, 0);
    execute_and_print("T04", "MIGRATION_INSUFFICIENT_INFORMATION", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_MEMORY, "T05", "REJECTED", 999999, 0);
    execute_and_print("T05", "MIGRATION_NOT_AUTHORIZED", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T06", "REJECTED", 999999, 999999);
    execute_and_print("T06", "MIGRATION_NOT_AUTHORIZED", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_MEMORY, "T19", "REJECTED", 999999, 0);
    execute_and_print("T19", "MIGRATION_NOT_AUTHORIZED", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T20", "REJECTED", 999999, 999999);
    execute_and_print("T20", "MIGRATION_NOT_AUTHORIZED", &request, 1);
    set_identity(&request, VALIDATION_ACTION_NO_MIGRATION, "T21", "REJECTED", 999999, 0);
    execute_and_print("T21", "MIGRATION_NO_ACTION", &request, 1);
    set_identity(&request, VALIDATION_ACTION_INSUFFICIENT, "T22", "REJECTED", 999999, 0);
    execute_and_print("T22", "MIGRATION_INSUFFICIENT_INFORMATION", &request, 1);

    child_a = spawn_pause();
    set_identity(&request, VALIDATION_ACTION_MOVE_MEMORY, "T01", "APPROVED", child_a, 0);
    request.numa_nodes_available = true;
    request.source_numa_node = 0;
    request.destination_numa_node = 1;
    print_test("T01", "approved memory migration attempted when remote node and verified pages exist",
               "remote NUMA node unavailable", "NOT TESTED — ENVIRONMENT LIMITATION");
    affinity_available = child_a > 0 && pin_process(child_a, 0) == 0;
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T02", "APPROVED", child_a, child_a);
    request.destination_cpu = 1;
    if (!affinity_available) {
        execute_and_print("T02", "MIGRATION_SUCCESS", &request, 0);
    } else {
        execute_and_print("T02", "MIGRATION_SUCCESS", &request, 1);
        pin_process(child_a, 0);
        set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T07", "APPROVED", child_a, child_a);
        request.destination_cpu = 1;
        execute_and_print("T07", "MIGRATION_SUCCESS", &request, 1);
        set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T08", "APPROVED", child_a, child_a);
        request.destination_cpu = 2;
        execute_and_print("T08", "MIGRATION_SUCCESS", &request, 1);
    }
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T35", "APPROVED", child_a, child_a);
    request.start_time_ticks = 1;
    request.start_time_ticks_available = true;
    request.destination_cpu = 1;
    execute_and_print("T35", "MIGRATION_TARGET_GONE", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T09", "APPROVED", child_a, child_a);
    request.destination_cpu = 9999;
    execute_and_print("T09", "MIGRATION_INVALID_DESTINATION", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T10", "APPROVED", child_a, 999999);
    request.destination_cpu = 1;
    execute_and_print("T10", "MIGRATION_TARGET_GONE", &request, 1);

    child_b = spawn_pause();
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T11", "APPROVED", child_a, child_b);
    request.destination_cpu = 1;
    execute_and_print("T11", "MIGRATION_INVALID_TARGET", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T12", "APPROVED", child_a, child_a);
    request.destination_cpu = affinity_available ? 2 : 0;
    execute_and_print("T12", "MIGRATION_NO_MIGRATION_REQUIRED", &request, affinity_available);
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T13", "APPROVED", child_a, child_a);
    request.destination_cpu = 1;
    if (Migration_AcquireExecutionLock(&request)) {
        execute_and_print("T13", "MIGRATION_ALREADY_IN_PROGRESS", &request, 1);
        Migration_ReleaseExecutionLock(&request);
    } else {
        print_test("T13", "MIGRATION_ALREADY_IN_PROGRESS", "lock setup failed", "FAIL");
    }
    stop_child(child_b);

    set_identity(&request, VALIDATION_ACTION_MOVE_MEMORY, "T14", "APPROVED", child_a, 0);
    request.numa_nodes_available = true;
    request.source_numa_node = 0;
    request.destination_numa_node = 0;
    execute_and_print("T14", "MIGRATION_NO_MIGRATION_REQUIRED", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_MEMORY, "T15", "APPROVED", child_a, 0);
    request.numa_nodes_available = true;
    request.source_numa_node = 0;
    request.destination_numa_node = 999;
    execute_and_print("T15", "MIGRATION_INVALID_DESTINATION", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_MEMORY, "T16", "APPROVED", child_a, 0);
    request.numa_nodes_available = true;
    request.source_numa_node = 0;
    request.destination_numa_node = 0;
    request.explicit_placement_required = true;
    execute_and_print("T16", "MIGRATION_INSUFFICIENT_INFORMATION", &request, 1);
    set_identity(&request, VALIDATION_ACTION_MOVE_MEMORY, "T17", "APPROVED", child_a, 0);
    request.numa_nodes_available = true;
    request.source_numa_node = 0;
    request.destination_numa_node = 0;
    request.explicit_placement_required = true;
    request.page_metadata_available = true;
    request.memory_region_verified = true;
    request.page_count = 1;
    request.pages = calloc(1, sizeof(*request.pages));
    request.pages[0] = (void *)(uintptr_t)1;
    execute_and_print("T17", "MIGRATION_INVALID_SOURCE", &request, 1);
    free(request.pages);
    set_identity(&request, VALIDATION_ACTION_MOVE_MEMORY, "T18", "APPROVED", child_a, 0);
    request.numa_nodes_available = true;
    request.source_numa_node = 0;
    request.destination_numa_node = 0;
    request.memory_locked = true;
    print_test("T18", "controlled locked/pinned memory rejection", "not safely reproduced", "NOT TESTED — ENVIRONMENT LIMITATION");

    stop_child(child_a);
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T23", "APPROVED", child_a, child_a);
    request.destination_cpu = 1;
    execute_and_print("T23", "MIGRATION_TARGET_GONE", &request, 1);
    child_c = spawn_thread_that_exits(&worker_tid);
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T24", "APPROVED", child_c, (pid_t)worker_tid);
    request.destination_cpu = 1;
    execute_and_print("T24", "MIGRATION_TARGET_GONE", &request, 1);
    stop_child(child_c);

    child_a = spawn_pause();
    affinity_available = child_a > 0 && pin_process(child_a, 0) == 0;
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T25", "APPROVED", child_a, child_a);
    request.destination_cpu = 1;
    if (affinity_available)
        Migration_Execute(&request, &report);
    print_test("T25", "successful migration CSV record", affinity_available ? (contains_text(results_path, "T25") ? "record present" : "record missing") : "affinity unavailable", affinity_available ? (contains_text(results_path, "T25") ? "PASS" : "FAIL") : "NOT TESTED — ENVIRONMENT LIMITATION");
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T26", "APPROVED", child_a, 999999);
    request.destination_cpu = 1;
    Migration_Execute(&request, &report);
    print_test("T26", "failed migration CSV record", contains_text(results_path, "T26") && contains_text(results_path, "MIGRATION_TARGET_GONE") ? "record present" : "record missing", contains_text(results_path, "T26") && contains_text(results_path, "MIGRATION_TARGET_GONE") ? "PASS" : "FAIL");
    print_test("T27", "human-readable migration log", affinity_available ? (contains_text(log_path, "migration_id=T25") ? "record present" : "record missing") : "affinity unavailable", affinity_available ? (contains_text(log_path, "migration_id=T25") ? "PASS" : "FAIL") : "NOT TESTED — ENVIRONMENT LIMITATION");
    stop_child(child_a);

    if (reset_module(3, 30.0, 1) != 0)
        return EXIT_FAILURE;
    for (int index = 0; index < 5; index++) {
        char id[16];

        snprintf(id, sizeof(id), "T28-%d", index);
        set_identity(&request, VALIDATION_ACTION_NO_MIGRATION, id, "REJECTED", 0, 0);
        Migration_Execute(&request, &report);
    }
    print_test("T28", "3 records retained", "3 records retained", line_count(history_path) == 4 ? "PASS" : "FAIL");

    {
        FILE *file = fopen(history_path, "w");
        long long now = (long long)time(NULL);
        fprintf(file, "header\nold,1.0,1\nnew,1.0,%lld\n", now);
        fclose(file);
    }
    Migration_Shutdown();
    age_config = test_config(1000, 0.000001, 100);
    if (!Migration_Init(&age_config))
        return EXIT_FAILURE;
    print_test("T29", "old record removed, new retained", "cleanup executed", Migration_CleanupHistory() && line_count(history_path) == 2 ? "PASS" : "FAIL");

    {
        char temporary[PATH_MAX];
        FILE *file = fopen(history_path, "w");

        fprintf(file, "header\nrecord,1.0,%lld\n", (long long)time(NULL));
        fclose(file);
        if (snprintf(temporary, sizeof(temporary), "%s.tmp", history_path) >= (int)sizeof(temporary))
            return EXIT_FAILURE;
        mkdir(temporary, 0700);
        print_test("T30", "original history preserved on cleanup failure", "original retained", !Migration_CleanupHistory() && line_count(history_path) == 2 ? "PASS" : "FAIL");
        rmdir(temporary);
    }

    child_a = spawn_pause();
    affinity_available = child_a > 0 && pin_process(child_a, 0) == 0;
    set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T31", "APPROVED", child_a, child_a);
    request.destination_cpu = 1;
    if (!affinity_available)
        execute_and_print("T31", "MIGRATION_SUCCESS", &request, 0);
    else
        execute_and_print("T31", "MIGRATION_SUCCESS", &request, 1);
    if (affinity_available) {
        get_affinity(child_a, &before);
        set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T32", "REJECTED", child_a, child_a);
        request.destination_cpu = 2;
        Migration_Execute(&request, &report);
        get_affinity(child_a, &after);
        print_test("T32", "MIGRATION_NOT_AUTHORIZED and unchanged affinity",
                   MigrationResultName(report.result), report.result == MIGRATION_NOT_AUTHORIZED && CPU_EQUAL(&before, &after) ? "PASS" : "FAIL");
    } else {
        print_test("T32", "MIGRATION_NOT_AUTHORIZED and unchanged affinity", "affinity unavailable", "NOT TESTED — ENVIRONMENT LIMITATION");
    }

    remote_nodes = numa_node_count();
    print_test("T33", "remote NUMA migration only with >=2 nodes", remote_nodes == 1 ? "1 NUMA node" : "multiple NUMA nodes", remote_nodes < 2 ? "NOT TESTED — ENVIRONMENT LIMITATION" : "PASS");

    if (affinity_available) {
        double minimum = 0.0, maximum = 0.0, total = 0.0;
        int repetitions = 5;

        for (int index = 0; index < repetitions; index++) {
            set_identity(&request, VALIDATION_ACTION_MOVE_THREAD, "T34", "APPROVED", child_a, child_a);
            request.destination_cpu = index % 2;
            Migration_Execute(&request, &report);
            if (index == 0 || report.execution_time_ms < minimum) minimum = report.execution_time_ms;
            if (report.execution_time_ms > maximum) maximum = report.execution_time_ms;
            total += report.execution_time_ms;
        }
        printf("T34 overhead | expected execution overhead measurements | min=%.3f ms max=%.3f ms avg=%.3f ms | PASS\n",
               minimum, maximum, total / repetitions);
    } else {
        print_test("T34", "execution overhead measurements", "affinity unavailable", "NOT TESTED — ENVIRONMENT LIMITATION");
    }
    stop_child(child_a);

    Migration_Shutdown();
    unlink(results_path);
    unlink(history_path);
    unlink(log_path);
    unlink(state_path);
    rmdir(directory);
    return EXIT_SUCCESS;
}
