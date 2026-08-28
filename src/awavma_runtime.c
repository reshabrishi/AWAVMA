#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"

#include "application_manager.h"
#include "classifier.h"
#include "monitor_profile.h"
#include "worker_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ROOT_DIR "results/runtime"
#define DEFAULT_BIN_DIR "bin"
#define DEFAULT_PHASE_CONFIG "config/awavma.conf"
#define DEFAULT_MONITOR_INTERVAL_MS 100U
#define DEFAULT_EVALUATION_INTERVAL_MS 1000U
#define DEFAULT_MAX_APPLICATIONS 256U
#define DEFAULT_WORKERS 2U
#define DEFAULT_QUEUE_CAPACITY 64U

static size_t split_csv(char *line, char **fields, size_t capacity);
static int column_index(char **fields, size_t count, const char *name);

struct awavma_runtime {
    awavma_runtime_config_t config;
    application_manager_t *manager;
    worker_pool_t *pool;
    runtime_monitor_t *monitor;
    awavma_runtime_record_t *records;
    size_t record_count;
    char manager_log_path[4096];
    char manager_results_path[4096];
    char manager_table_path[4096];
    char monitor_dir[4096];
    char monitor_results_path[4096];
    char monitor_log_path[4096];
    char runtime_results_path[4096];
    bool initialized;
    _Atomic bool stop_requested;
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

#ifdef AWAVMA_PROFILE
static uint64_t monotonic_ns(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}
#endif

static int path_join(char *buffer, size_t size, const char *left, const char *right)
{
    int written = snprintf(buffer, size, "%s/%s", left, right);

    return written < 0 || (size_t)written >= size ? -1 : 0;
}

static int make_directory(const char *path)
{
    char copy[4096];
    size_t length;

    if (path == NULL || *path == '\0' || strlen(path) >= sizeof(copy))
        return -1;
    snprintf(copy, sizeof(copy), "%s", path);
    length = strlen(copy);
    if (length > 1 && copy[length - 1] == '/')
        copy[length - 1] = '\0';
    for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    return mkdir(copy, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static bool safe_app_id(const char *app_id)
{
    if (app_id == NULL || *app_id == '\0')
        return false;
    for (const unsigned char *cursor = (const unsigned char *)app_id; *cursor != '\0'; cursor++)
        if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_' || *cursor == '-'))
            return false;
    return true;
}

static size_t csv_row_count(const char *path)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    size_t count = 0;

    if (file == NULL)
        return 0;
    if (getline(&line, &capacity, file) >= 0)
        while (getline(&line, &capacity, file) >= 0)
            if (line[0] != '\n' && line[0] != '\0')
                count++;
    free(line);
    fclose(file);
    return count;
}

static int copy_csv_delta(const char *source, const char *destination, size_t start_row)
{
    FILE *input = fopen(source, "r");
    FILE *output = NULL;
    char *line = NULL;
    size_t capacity = 0;
    size_t row = 0;
    int result = -1;

    if (input == NULL)
        return -1;
    output = fopen(destination, "w");
    if (output == NULL)
        goto cleanup;
    if (getline(&line, &capacity, input) < 0)
        goto cleanup;
    fputs(line, output);
    while (getline(&line, &capacity, input) >= 0) {
        if (row++ >= start_row)
            fputs(line, output);
    }
    result = ferror(input) || ferror(output) ? -1 : 0;

cleanup:
    free(line);
    fclose(input);
    if (output != NULL)
        fclose(output);
    return result;
}

/* One-shot Phase 3 sessions restart their local elapsed clock for every sample. */
static int normalize_monitoring_elapsed(const char *source, const char *destination,
                                        uint64_t interval_ms)
{
    FILE *input = fopen(source, "r");
    FILE *output = NULL;
    char *line = NULL;
    size_t capacity = 0;
    char *fields[64];
    int elapsed_column;
    size_t row = 0;
    int result = -1;

    if (input == NULL)
        return -1;
    output = fopen(destination, "w");
    if (output == NULL || getline(&line, &capacity, input) < 0)
        goto cleanup;
    fputs(line, output);
    elapsed_column = column_index(fields, split_csv(line, fields, 64), "elapsed_ms");
    if (elapsed_column < 0)
        goto cleanup;
    while (getline(&line, &capacity, input) >= 0) {
        char elapsed[32];
        size_t field_count = split_csv(line, fields, 64);

        if ((size_t)elapsed_column >= field_count)
            goto cleanup;
        snprintf(elapsed, sizeof(elapsed), "%llu",
                 (unsigned long long)(row++ * interval_ms));
        fields[elapsed_column] = elapsed;
        for (size_t index = 0; index < field_count; index++)
            fprintf(output, "%s%s", index == 0 ? "" : ",", fields[index]);
        fputc('\n', output);
    }
    result = ferror(input) || ferror(output) ? -1 : 0;

cleanup:
    free(line);
    fclose(input);
    if (output != NULL)
        fclose(output);
    return result;
}

static size_t split_csv(char *line, char **fields, size_t capacity)
{
    size_t count = 0;
    char *cursor = line;

    while (cursor != NULL && count < capacity) {
        fields[count++] = cursor;
        cursor = strchr(cursor, ',');
        if (cursor != NULL)
            *cursor++ = '\0';
    }
    if (count > 0) {
        char *last = fields[count - 1];
        last[strcspn(last, "\r\n")] = '\0';
    }
    return count;
}

static int column_index(char **fields, size_t count, const char *name)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(fields[index], name) == 0)
            return (int)index;
    return -1;
}

static const char *field_or_na(char **fields, size_t count, int index)
{
    return index >= 0 && (size_t)index < count && fields[index][0] != '\0' ? fields[index] : "NA";
}

/* Phase 6 requires operational evidence that Phase 3/4/5 do not currently emit. */
static int write_validation_input(const char *decision_path, const char *output_path,
                                  const awavma_runtime_record_t *record)
{
    static const char *header =
        "timestamp,migration_id,app_id,pid,entity_id,action,decision_status,classification,"
        "classifier_confidence,stability,sample_count,cpu_utilization,memory_utilization,"
        "concurrency,source_node,destination_node,predicted_gain,estimated_cost,page_locked,"
        "memory_pinned,cooldown_active,thread_locked,migration_in_progress,max_migrations_reached\n";
    FILE *input = fopen(decision_path, "r");
    FILE *output = NULL;
    char *line = NULL;
    size_t line_capacity = 0;
    char *fields[64];
    int timestamp;
    int app_id;
    int pid;
    int entity_id;
    int action;
    int status;
    int classification;
    size_t row = 0;
    int result = -1;

    if (input == NULL)
        return -1;
    output = fopen(output_path, "w");
    if (output == NULL || getline(&line, &line_capacity, input) < 0)
        goto cleanup;
    size_t field_count = split_csv(line, fields, 64);
    timestamp = column_index(fields, field_count, "timestamp");
    app_id = column_index(fields, field_count, "app_id");
    pid = column_index(fields, field_count, "pid");
    entity_id = column_index(fields, field_count, "entity_id");
    action = column_index(fields, field_count, "decision");
    status = column_index(fields, field_count, "status");
    classification = column_index(fields, field_count, "classification");
    if (timestamp < 0 || app_id < 0 || pid < 0 || entity_id < 0 || action < 0 || status < 0 ||
        classification < 0)
        goto cleanup;
    fputs(header, output);
    while (getline(&line, &line_capacity, input) >= 0) {
        char migration_id[128];

        field_count = split_csv(line, fields, 64);
        snprintf(migration_id, sizeof(migration_id), "m_%ld_%llu_%zu", (long)record->pid,
                 (unsigned long long)record->generation, row++);
        fprintf(output, "%s,%s,%s,%s,%s,%s,%s,%s,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA\n",
                field_or_na(fields, field_count, timestamp), migration_id,
                field_or_na(fields, field_count, app_id), field_or_na(fields, field_count, pid),
                field_or_na(fields, field_count, entity_id), field_or_na(fields, field_count, action),
                field_or_na(fields, field_count, status),
                field_or_na(fields, field_count, classification));
    }
    result = ferror(input) || ferror(output) ? -1 : 0;

cleanup:
    free(line);
    fclose(input);
    if (output != NULL)
        fclose(output);
    return result;
}

static bool csv_has_value(const char *path, const char *column, const char *value)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    char *fields[64];
    int target;
    bool found = false;

    if (file == NULL || getline(&line, &capacity, file) < 0) {
        if (file != NULL)
            fclose(file);
        free(line);
        return false;
    }
    target = column_index(fields, split_csv(line, fields, 64), column);
    while (target >= 0 && getline(&line, &capacity, file) >= 0) {
        size_t count = split_csv(line, fields, 64);

        if ((size_t)target < count && strcmp(fields[target], value) == 0) {
            found = true;
            break;
        }
    }
    free(line);
    fclose(file);
    return found;
}

static int run_program(const awavma_runtime_t *runtime, const char *cycle_dir,
                       const char *phase, const char *application, pid_t pid,
                       uint64_t generation, char *const arguments[])
{
    char stdout_path[4096];
    char stderr_path[4096];
    char child_profile_path[4096];
    char prepare_component[64];
    char fork_component[64];
    pid_t child;
    int status;
    int stdout_fd;
    int stderr_fd;
    monitor_profile_scope_t prepare_profile;
    monitor_profile_scope_t fork_profile;

    snprintf(prepare_component, sizeof(prepare_component), "%s_parent_prepare", phase);
    snprintf(fork_component, sizeof(fork_component), "%s_fork_wait_wall", phase);
    monitor_profile_scope_begin(&prepare_profile, "pipeline", prepare_component, application, pid, generation);
    if (snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout.log", cycle_dir, phase) >=
            (int)sizeof(stdout_path) ||
        snprintf(stderr_path, sizeof(stderr_path), "%s/%s.stderr.log", cycle_dir, phase) >=
            (int)sizeof(stderr_path) ||
        snprintf(child_profile_path, sizeof(child_profile_path), "%s/%s.profile.csv", cycle_dir, phase) >=
            (int)sizeof(stderr_path))
    {
        monitor_profile_scope_end(&prepare_profile, "ERROR");
        return -1;
    }
    stdout_fd = open(stdout_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    stderr_fd = open(stderr_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (stdout_fd < 0 || stderr_fd < 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        monitor_profile_scope_end(&prepare_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&prepare_profile, "OK");
    monitor_profile_scope_begin(&fork_profile, "pipeline", fork_component, application, pid, generation);
    child = fork();
    if (child == 0) {
        dup2(stdout_fd, STDOUT_FILENO);
        dup2(stderr_fd, STDERR_FILENO);
        close(stdout_fd);
        close(stderr_fd);
        if (setenv("AWAVMA_PROFILE_PATH", child_profile_path, 1) != 0)
            _exit(127);
        execv(arguments[0], arguments);
        _exit(127);
    }
    close(stdout_fd);
    close(stderr_fd);
    if (child < 0) {
        monitor_profile_scope_end(&fork_profile, "ERROR");
        return -1;
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) {
            monitor_profile_scope_end(&fork_profile, "ERROR");
            return -1;
        }
    (void)runtime;
    int result = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    monitor_profile_scope_end(&fork_profile, result == 0 ? "OK" : "ERROR");
    return result;
}

static int run_classifier_in_process(const char *cycle_dir, const classifier_config_t *config,
                                     classifier_summary_t *summary)
{
    char stdout_path[4096];
    char stderr_path[4096];
    int stdout_fd;
    int stderr_fd;
    int saved_stdout;
    int saved_stderr;
    int result;

    if (snprintf(stdout_path, sizeof(stdout_path), "%s/phase4.stdout.log", cycle_dir) >= (int)sizeof(stdout_path) ||
        snprintf(stderr_path, sizeof(stderr_path), "%s/phase4.stderr.log", cycle_dir) >= (int)sizeof(stderr_path))
        return -1;
    stdout_fd = open(stdout_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    stderr_fd = open(stderr_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (stdout_fd < 0 || stderr_fd < 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout < 0 || saved_stderr < 0 || dup2(stdout_fd, STDOUT_FILENO) < 0 ||
        dup2(stderr_fd, STDERR_FILENO) < 0) {
        if (saved_stdout >= 0)
            dup2(saved_stdout, STDOUT_FILENO);
        if (saved_stderr >= 0)
            dup2(saved_stderr, STDERR_FILENO);
        if (saved_stdout >= 0) close(saved_stdout);
        if (saved_stderr >= 0) close(saved_stderr);
        close(stdout_fd);
        close(stderr_fd);
        return -1;
    }
    close(stdout_fd);
    close(stderr_fd);
    fprintf(stdout, "AWAVMA in-process Phase 4 classifier\n");
    result = classifier_run(config, summary);
    fflush(stdout);
    fflush(stderr);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);
    return result;
}

static awavma_runtime_record_t *runtime_record(awavma_runtime_t *runtime,
                                                const runtime_monitor_record_t *monitor_record)
{
    for (size_t index = 0; index < runtime->record_count; index++)
        if (runtime->records[index].pid == monitor_record->pid &&
            runtime->records[index].start_time_ticks == monitor_record->start_time_ticks &&
            strcmp(runtime->records[index].app_id, monitor_record->app_id) == 0)
            return &runtime->records[index];
    if (runtime->record_count >= runtime->config.max_applications)
        return NULL;
    awavma_runtime_record_t *record = &runtime->records[runtime->record_count++];
    memset(record, 0, sizeof(*record));
    snprintf(record->app_id, sizeof(record->app_id), "%s", monitor_record->app_id);
    record->pid = monitor_record->pid;
    record->start_time_ticks = monitor_record->start_time_ticks;
    record->status = AWAVMA_RUNTIME_MONITORING;
    snprintf(record->detail, sizeof(record->detail), "monitoring");
    return record;
}

static void set_status(awavma_runtime_record_t *record, awavma_runtime_status_t status,
                       const char *detail)
{
    record->status = status;
    snprintf(record->detail, sizeof(record->detail), "%s", detail);
}

static int write_runtime_results(const awavma_runtime_t *runtime)
{
    char temporary[4096];
    FILE *file;

    if (snprintf(temporary, sizeof(temporary), "%s.tmp", runtime->runtime_results_path) >=
        (int)sizeof(temporary))
        return -1;
    file = fopen(temporary, "w");
    if (file == NULL)
        return -1;
    fprintf(file, "app_id,pid,start_time_ticks,generation,phase3_samples,phase5_committed_rows,status,detail\n");
    for (size_t index = 0; index < runtime->record_count; index++) {
        const awavma_runtime_record_t *record = &runtime->records[index];

        fprintf(file, "%s,%ld,%llu,%llu,%zu,%zu,%s,%s\n", record->app_id, (long)record->pid,
                (unsigned long long)record->start_time_ticks,
                (unsigned long long)record->generation, record->phase3_samples,
                record->phase5_committed_rows, awavma_runtime_status_name(record->status),
                record->detail);
    }
    if (fclose(file) != 0 || rename(temporary, runtime->runtime_results_path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int process_application(awavma_runtime_t *runtime, awavma_runtime_record_t *record,
                               uint64_t pipeline_ready_ns)
{
    char phase3_path[4096];
    char app_dir[4096];
    char cycles_dir[4096];
    char cycle_dir[4096];
    char classification_path[4096];
    char normalized_monitoring_path[4096];
    char classification_delta_path[4096];
    char decision_path[4096];
    char validation_input_path[4096];
    char validation_path[4096];
    char state_dir[4096];
    char history_dir[4096];
    char log_path[4096];
    char classifier[4096];
    char decision[4096];
    char validation[4096];
    char generation[64];
    size_t samples;
    monitor_profile_scope_t coordinator_profile;
    monitor_profile_scope_t scan_profile;
    monitor_profile_scope_t artifact_profile;
    monitor_profile_scope_t normalize_profile;
    monitor_profile_scope_t delta_profile;
    monitor_profile_scope_t adapter_profile;
    monitor_profile_scope_t result_profile;

    if (!safe_app_id(record->app_id) ||
        snprintf(phase3_path, sizeof(phase3_path), "%s/phase3/%s_monitoring.csv",
                 runtime->config.root_dir, record->app_id) >= (int)sizeof(phase3_path)) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "invalid Phase 3 artifact path");
        return -1;
    }
    monitor_profile_scope_begin(&scan_profile, "pipeline", "phase3_artifact_scan", record->app_id,
                                record->pid, record->generation + 1);
    samples = csv_row_count(phase3_path);
    monitor_profile_scope_end(&scan_profile, "OK");
    record->phase3_samples = samples;
    if (record->status == AWAVMA_RUNTIME_AMBIGUOUS)
        return 0;
    if (samples <= record->phase5_committed_rows)
        return 0;
#ifdef AWAVMA_PROFILE
    monitor_profile_event("pipeline", "pipeline_serial_wait", record->app_id, record->pid,
                          record->generation + 1, pipeline_ready_ns, monotonic_ns(), "OK");
#else
    (void)pipeline_ready_ns;
#endif
    monitor_profile_scope_begin(&coordinator_profile, "coordinator", "coordinator_application_total",
                                record->app_id, record->pid, record->generation + 1);
    monitor_profile_scope_begin(&artifact_profile, "coordinator", "coordinator_artifact_prepare",
                                record->app_id, record->pid, record->generation + 1);
    if (snprintf(app_dir, sizeof(app_dir), "%s/apps/%s", runtime->config.root_dir, record->app_id) >=
            (int)sizeof(app_dir) ||
        snprintf(generation, sizeof(generation), "%llu", (unsigned long long)(record->generation + 1)) >=
            (int)sizeof(generation) ||
        path_join(cycles_dir, sizeof(cycles_dir), app_dir, "cycles") != 0 ||
        path_join(cycle_dir, sizeof(cycle_dir), cycles_dir, generation) != 0 ||
        path_join(state_dir, sizeof(state_dir), app_dir, "state") != 0 ||
        path_join(history_dir, sizeof(history_dir), app_dir, "history") != 0 ||
        path_join(log_path, sizeof(log_path), app_dir, "decision.log") != 0 ||
        make_directory(cycle_dir) != 0 || make_directory(state_dir) != 0 ||
        make_directory(history_dir) != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "cannot create application artifacts");
        monitor_profile_scope_end(&artifact_profile, "ERROR");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    if (path_join(normalized_monitoring_path, sizeof(normalized_monitoring_path), cycle_dir,
                  "monitoring_normalized.csv") != 0 ||
        path_join(classification_path, sizeof(classification_path), cycle_dir, "classification_full.csv") != 0 ||
        path_join(classification_delta_path, sizeof(classification_delta_path), cycle_dir, "classification_delta.csv") != 0 ||
        path_join(decision_path, sizeof(decision_path), cycle_dir, "decision.csv") != 0 ||
        path_join(validation_input_path, sizeof(validation_input_path), cycle_dir, "validation_input.csv") != 0 ||
        path_join(validation_path, sizeof(validation_path), cycle_dir, "validation.csv") != 0 ||
        path_join(classifier, sizeof(classifier), runtime->config.bin_dir, "classifier") != 0 ||
        path_join(decision, sizeof(decision), runtime->config.bin_dir, "decision") != 0 ||
        path_join(validation, sizeof(validation), runtime->config.bin_dir, "validation") != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "artifact path exceeds limit");
        monitor_profile_scope_end(&artifact_profile, "ERROR");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&artifact_profile, "OK");
    char *classifier_args[] = {classifier, "--input", normalized_monitoring_path, "--output", classification_path, NULL};
    monitor_profile_scope_begin(&normalize_profile, "pipeline", "monitoring_normalize_io", record->app_id,
                                record->pid, record->generation + 1);
    if (normalize_monitoring_elapsed(phase3_path, normalized_monitoring_path,
                                    runtime->config.monitor_interval_ms) != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "Phase 4 failed");
        monitor_profile_scope_end(&normalize_profile, "ERROR");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&normalize_profile, "OK");
    monitor_profile_scope_t phase4_profile;
    monitor_profile_scope_begin(&phase4_profile, "pipeline", "phase4_execution", record->app_id,
                                record->pid, record->generation + 1);
    int phase4_result;
    if (runtime->config.phase4_mode == AWAVMA_PHASE4_IN_PROCESS) {
        classifier_config_t classifier_config;
        classifier_summary_t classifier_summary;
        monitor_profile_scope_t direct_profile;

        classifier_config_default(&classifier_config);
        classifier_config.input_path = normalized_monitoring_path;
        classifier_config.output_path = classification_path;
        monitor_profile_scope_begin(&direct_profile, "pipeline", "phase4_direct_api_total",
                                    record->app_id, record->pid, record->generation + 1);
        phase4_result = run_classifier_in_process(cycle_dir, &classifier_config, &classifier_summary);
        monitor_profile_scope_end(&direct_profile, phase4_result == 0 ? "OK" : "ERROR");
    } else {
        phase4_result = run_program(runtime, cycle_dir, "phase4", record->app_id, record->pid,
                                    record->generation + 1, classifier_args);
    }
    monitor_profile_scope_end(&phase4_profile, phase4_result == 0 ? "OK" : "ERROR");
    if (phase4_result != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "Phase 4 failed");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_begin(&delta_profile, "pipeline", "classification_delta_io", record->app_id,
                                record->pid, record->generation + 1);
    int delta_result = copy_csv_delta(classification_path, classification_delta_path,
                                      record->phase5_committed_rows);
    monitor_profile_scope_end(&delta_profile, delta_result == 0 ? "OK" : "ERROR");
    if (delta_result != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "Phase 4 failed");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    char *decision_args[] = {decision, "--input", classification_delta_path, "--output", decision_path,
                             "--app-id", record->app_id, "--state-dir", state_dir,
                             "--history-dir", history_dir, "--log", log_path, NULL};
    record->generation++;
    monitor_profile_scope_t phase5_profile;
    monitor_profile_scope_begin(&phase5_profile, "pipeline", "phase5_execution", record->app_id,
                                record->pid, record->generation);
    int phase5_result = run_program(runtime, cycle_dir, "phase5", record->app_id, record->pid,
                                    record->generation, decision_args);
    monitor_profile_scope_end(&phase5_profile, phase5_result == 0 ? "OK" : "ERROR");
    if (phase5_result != 0) {
        set_status(record, AWAVMA_RUNTIME_AMBIGUOUS, "Phase 5 outcome is unknown");
        monitor_profile_scope_end(&coordinator_profile, "AMBIGUOUS");
        return -1;
    }
    record->phase5_committed_rows = samples;
    monitor_profile_scope_begin(&adapter_profile, "pipeline", "validation_input_adapt_io", record->app_id,
                                record->pid, record->generation);
    if (write_validation_input(decision_path, validation_input_path, record) != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "cannot adapt Phase 5 output for Phase 6");
        monitor_profile_scope_end(&adapter_profile, "ERROR");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    monitor_profile_scope_end(&adapter_profile, "OK");
    char validation_history[4096];
    char validation_log[4096];
    if (path_join(validation_history, sizeof(validation_history), history_dir, "validation.csv") != 0 ||
        path_join(validation_log, sizeof(validation_log), app_dir, "validation.log") != 0) {
        set_status(record, AWAVMA_RUNTIME_ERROR, "validation path exceeds limit");
        monitor_profile_scope_end(&coordinator_profile, "ERROR");
        return -1;
    }
    char *validation_args[] = {validation, "--input", validation_input_path, "--output", validation_path,
                               "--history", validation_history, "--log", validation_log,
                               "--config", (char *)runtime->config.phase_config_path, NULL};
    monitor_profile_scope_t phase6_profile;
    monitor_profile_scope_begin(&phase6_profile, "pipeline", "phase6_execution", record->app_id,
                                record->pid, record->generation);
    int phase6_result = run_program(runtime, cycle_dir, "phase6", record->app_id, record->pid,
                                    record->generation, validation_args);
    monitor_profile_scope_end(&phase6_profile, phase6_result == 0 ? "OK" : "ERROR");
    if (phase6_result != 0) {
        set_status(record, AWAVMA_RUNTIME_AMBIGUOUS, "Phase 6 outcome is unknown");
        monitor_profile_scope_end(&coordinator_profile, "AMBIGUOUS");
        return -1;
    }
    monitor_profile_scope_begin(&result_profile, "pipeline", "pipeline_result_parse_io", record->app_id,
                                record->pid, record->generation);
    bool insufficient = csv_has_value(decision_path, "decision", "INSUFFICIENT_DECISION_SIGNAL");
    bool approved = !insufficient && csv_has_value(validation_path, "final_decision", "APPROVED");
    monitor_profile_scope_end(&result_profile, "OK");
    if (insufficient)
        set_status(record, AWAVMA_RUNTIME_INSUFFICIENT, "unavailable decision evidence");
    else if (approved)
        set_status(record, AWAVMA_RUNTIME_MIGRATION_METADATA_UNAVAILABLE,
                   "Phase 7 disabled: execution metadata unavailable");
    else
        set_status(record, AWAVMA_RUNTIME_REJECTED, "Phase 6 did not approve migration");
#ifdef AWAVMA_PROFILE
    monitor_profile_counter("pipeline", "pipeline_completion", record->app_id, record->pid,
                            record->generation, 1, awavma_runtime_status_name(record->status));
#endif
    monitor_profile_scope_end(&coordinator_profile, "OK");
    return 0;
}

static int process_pipeline(awavma_runtime_t *runtime)
{
    runtime_monitor_record_t *monitor_records;
    size_t monitor_count;
    uint64_t pipeline_ready_ns = 0;
    monitor_profile_scope_t pipeline_profile;
    monitor_profile_scope_t snapshot_profile;
    monitor_profile_scope_t publish_profile;

#ifdef AWAVMA_PROFILE
    pipeline_ready_ns = monotonic_ns();
#endif
    monitor_profile_scope_begin(&pipeline_profile, "pipeline", "pipeline_serial_total", NULL, -1, 0);

    monitor_profile_scope_begin(&snapshot_profile, "pipeline", "pipeline_snapshot_prepare", NULL, -1, 0);
    monitor_records = calloc(runtime->config.max_applications, sizeof(*monitor_records));
    if (monitor_records == NULL) {
        monitor_profile_scope_end(&snapshot_profile, "ERROR");
        monitor_profile_scope_end(&pipeline_profile, "ERROR");
        return ENOMEM;
    }
    monitor_count = runtime_monitor_snapshot(runtime->monitor, monitor_records,
                                              runtime->config.max_applications);
    monitor_profile_scope_end(&snapshot_profile, "OK");
    for (size_t index = 0; index < monitor_count; index++) {
        awavma_runtime_record_t *record = runtime_record(runtime, &monitor_records[index]);
        application_manager_record_t application;

        if (record == NULL)
            continue;
        if (!application_manager_lookup_identity(runtime->manager, record->pid,
                                                 record->start_time_ticks, &application) ||
            application.status != APPLICATION_MANAGER_ACTIVE) {
            set_status(record, AWAVMA_RUNTIME_TARGET_GONE, "identity no longer active");
            continue;
        }
        if (monitor_records[index].status == RUNTIME_MONITOR_TARGET_GONE ||
            monitor_records[index].status == RUNTIME_MONITOR_TERMINATED) {
            set_status(record, AWAVMA_RUNTIME_TARGET_GONE, "monitor target terminated");
            continue;
        }
        if (monitor_records[index].status == RUNTIME_MONITOR_ERROR) {
            set_status(record, AWAVMA_RUNTIME_ERROR, "Phase 3 sampling failed");
            continue;
        }
        (void)process_application(runtime, record, pipeline_ready_ns);
    }
    free(monitor_records);
    monitor_profile_scope_begin(&publish_profile, "pipeline", "runtime_results_publish_io", NULL, -1, 0);
    int result = write_runtime_results(runtime) == 0 ? 0 : EIO;
    monitor_profile_scope_end(&publish_profile, result == 0 ? "OK" : "ERROR");
    monitor_profile_scope_end(&pipeline_profile, result == 0 ? "OK" : "ERROR");
    return result;
}

void awavma_runtime_config_default(awavma_runtime_config_t *config)
{
    if (config == NULL)
        return;
    config->monitor_interval_ms = DEFAULT_MONITOR_INTERVAL_MS;
    config->discovery_interval_ms = 250;
    config->evaluation_interval_ms = DEFAULT_EVALUATION_INTERVAL_MS;
    config->max_applications = DEFAULT_MAX_APPLICATIONS;
    config->worker_count = DEFAULT_WORKERS;
    config->queue_capacity = DEFAULT_QUEUE_CAPACITY;
    config->phase4_mode = AWAVMA_PHASE4_SUBPROCESS;
    config->root_dir = DEFAULT_ROOT_DIR;
    config->bin_dir = DEFAULT_BIN_DIR;
    config->phase_config_path = DEFAULT_PHASE_CONFIG;
    application_discovery_config_default(&config->discovery_config);
    config->application_filter = NULL;
    config->application_filter_context = NULL;
}

const char *awavma_runtime_status_name(awavma_runtime_status_t status)
{
    switch (status) {
    case AWAVMA_RUNTIME_MONITORING: return "MONITORING";
    case AWAVMA_RUNTIME_INSUFFICIENT: return "INSUFFICIENT";
    case AWAVMA_RUNTIME_REJECTED: return "REJECTED";
    case AWAVMA_RUNTIME_MIGRATION_METADATA_UNAVAILABLE: return "MIGRATION_METADATA_UNAVAILABLE";
    case AWAVMA_RUNTIME_FEEDBACK_PENDING: return "FEEDBACK_PENDING";
    case AWAVMA_RUNTIME_TARGET_GONE: return "TARGET_GONE";
    case AWAVMA_RUNTIME_ERROR: return "ERROR";
    case AWAVMA_RUNTIME_AMBIGUOUS: return "AMBIGUOUS";
    default: return "UNKNOWN";
    }
}

awavma_runtime_t *awavma_runtime_create(void)
{
    return calloc(1, sizeof(awavma_runtime_t));
}

int awavma_runtime_init(awavma_runtime_t *runtime, const awavma_runtime_config_t *config)
{
    awavma_runtime_config_t defaults;
    application_manager_config_t manager_config;
    worker_pool_config_t pool_config;
    runtime_monitor_config_t monitor_config;
    char path[4096];

    if (runtime == NULL)
        return EINVAL;
    if (config == NULL) {
        awavma_runtime_config_default(&defaults);
        config = &defaults;
    }
    if (config->monitor_interval_ms == 0 || config->evaluation_interval_ms == 0 ||
        config->max_applications == 0 || config->worker_count == 0 || config->queue_capacity == 0 ||
        (config->phase4_mode != AWAVMA_PHASE4_SUBPROCESS &&
         config->phase4_mode != AWAVMA_PHASE4_IN_PROCESS) ||
        config->root_dir == NULL || config->bin_dir == NULL || config->phase_config_path == NULL)
        return EINVAL;
    memset(runtime, 0, sizeof(*runtime));
    runtime->config = *config;
    if (make_directory(config->root_dir) != 0 ||
        snprintf(path, sizeof(path), "%s/logs", config->root_dir) >= (int)sizeof(path) ||
        make_directory(path) != 0 ||
        snprintf(path, sizeof(path), "%s/phase3", config->root_dir) >= (int)sizeof(path) ||
        make_directory(path) != 0 ||
        snprintf(path, sizeof(path), "%s/apps", config->root_dir) >= (int)sizeof(path) ||
        make_directory(path) != 0)
        return EIO;
    runtime->records = calloc(config->max_applications, sizeof(*runtime->records));
    runtime->manager = application_manager_create();
    runtime->pool = worker_pool_create();
    runtime->monitor = runtime_monitor_create();
    if (runtime->records == NULL || runtime->manager == NULL || runtime->pool == NULL || runtime->monitor == NULL)
        goto fail;
    application_manager_config_default(&manager_config);
    manager_config.max_applications = config->max_applications;
    snprintf(runtime->manager_log_path, sizeof(runtime->manager_log_path),
             "%s/logs/application_manager.log", config->root_dir);
    snprintf(runtime->manager_results_path, sizeof(runtime->manager_results_path),
             "%s/application_manager_results.csv", config->root_dir);
    snprintf(runtime->manager_table_path, sizeof(runtime->manager_table_path),
             "%s/application_manager_table.csv", config->root_dir);
    manager_config.log_path = runtime->manager_log_path;
    manager_config.results_path = runtime->manager_results_path;
    manager_config.table_path = runtime->manager_table_path;
    if (application_manager_init(runtime->manager, &manager_config) != 0) {
        goto fail;
    }
    worker_pool_config_default(&pool_config);
    pool_config.worker_count = config->worker_count;
    pool_config.queue_capacity = config->queue_capacity;
    pool_config.log_path = NULL;
    if (worker_pool_init(runtime->pool, &pool_config) != WORKER_POOL_SUCCESS)
        goto fail;
    runtime_monitor_config_default(&monitor_config);
    monitor_config.monitor_interval_ms = config->monitor_interval_ms;
    monitor_config.discovery_interval_ms = config->discovery_interval_ms;
    monitor_config.max_applications = config->max_applications;
    snprintf(runtime->monitor_dir, sizeof(runtime->monitor_dir), "%s/phase3", config->root_dir);
    snprintf(runtime->monitor_results_path, sizeof(runtime->monitor_results_path),
             "%s/runtime_monitor_results.csv", config->root_dir);
    snprintf(runtime->monitor_log_path, sizeof(runtime->monitor_log_path),
             "%s/logs/runtime_monitor.log", config->root_dir);
    snprintf(runtime->runtime_results_path, sizeof(runtime->runtime_results_path),
             "%s/runtime_results.csv", config->root_dir);
    monitor_config.results_dir = runtime->monitor_dir;
    monitor_config.results_path = runtime->monitor_results_path;
    monitor_config.log_path = runtime->monitor_log_path;
    monitor_config.discovery_config = config->discovery_config;
    monitor_config.application_filter = config->application_filter;
    monitor_config.application_filter_context = config->application_filter_context;
    if (runtime_monitor_init(runtime->monitor, runtime->manager, runtime->pool, &monitor_config) != 0) {
        goto fail;
    }
    runtime->initialized = true;
    if (write_runtime_results(runtime) == 0)
        return 0;
    awavma_runtime_shutdown(runtime);
    return EIO;

fail:
    awavma_runtime_shutdown(runtime);
    return EIO;
}

int awavma_runtime_run_for(awavma_runtime_t *runtime, uint64_t duration_ms)
{
    uint64_t started;

    if (runtime == NULL || !runtime->initialized)
        return EINVAL;
    started = monotonic_ms();
    do {
        uint64_t slice = runtime->config.evaluation_interval_ms;

        if (duration_ms != 0 && monotonic_ms() - started < duration_ms &&
            duration_ms - (monotonic_ms() - started) < slice)
            slice = duration_ms - (monotonic_ms() - started);
        if (slice == 0)
            break;
        monitor_profile_scope_t cycle_profile;

        monitor_profile_scope_begin(&cycle_profile, "runtime", "runtime_cycle_total", NULL, -1, 0);
        if (runtime_monitor_run_for(runtime->monitor, slice) != 0) {
            monitor_profile_scope_end(&cycle_profile, "ERROR");
            return EIO;
        }
        if (process_pipeline(runtime) != 0) {
            monitor_profile_scope_end(&cycle_profile, "ERROR");
            return EIO;
        }
        monitor_profile_scope_end(&cycle_profile, "OK");
    } while (!atomic_load(&runtime->stop_requested) &&
             (duration_ms == 0 || monotonic_ms() - started < duration_ms));
    return 0;
}

void awavma_runtime_request_stop(awavma_runtime_t *runtime)
{
    if (runtime == NULL)
        return;
    atomic_store(&runtime->stop_requested, true);
    runtime_monitor_request_stop(runtime->monitor);
}

size_t awavma_runtime_snapshot(const awavma_runtime_t *runtime,
                               awavma_runtime_record_t *records, size_t capacity)
{
    size_t count;

    if (runtime == NULL || !runtime->initialized || records == NULL || capacity == 0)
        return 0;
    count = runtime->record_count < capacity ? runtime->record_count : capacity;
    memcpy(records, runtime->records, count * sizeof(*records));
    return count;
}

void awavma_runtime_shutdown(awavma_runtime_t *runtime)
{
    if (runtime == NULL)
        return;
    if (runtime->monitor != NULL)
        runtime_monitor_shutdown(runtime->monitor);
    if (runtime->pool != NULL)
        worker_pool_shutdown(runtime->pool);
    if (runtime->manager != NULL)
        application_manager_shutdown(runtime->manager);
    if (runtime->runtime_results_path[0] != '\0')
        (void)write_runtime_results(runtime);
    runtime->initialized = false;
}

void awavma_runtime_destroy(awavma_runtime_t *runtime)
{
    if (runtime == NULL)
        return;
    awavma_runtime_shutdown(runtime);
    runtime_monitor_destroy(runtime->monitor);
    worker_pool_destroy(runtime->pool);
    application_manager_destroy(runtime->manager);
    free(runtime->records);
    free(runtime);
}
