#define _POSIX_C_SOURCE 200809L

#include "decision.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    double factor_gain_memory;
    double factor_gain_thread;
    double memory_final;
    double thread_final;
    double margin;
    char decision[40];
    char margin_text[32];
    unsigned weight_version;
    unsigned bias_version;
} output_row_t;

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
    if (count > 0)
        fields[count - 1][strcspn(fields[count - 1], "\r\n")] = '\0';
    return count;
}

static int column(char **fields, size_t count, const char *name)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(fields[index], name) == 0)
            return (int)index;
    return -1;
}

static bool equal(double left, double right)
{
    return fabs(left - right) < 0.0000001;
}

static bool read_rows(const char *path, output_row_t rows[4])
{
    FILE *file = fopen(path, "r");
    char line[2048];
    char *fields[64];
    int gain_memory;
    int gain_thread;
    int memory_final;
    int thread_final;
    int margin;
    int decision;
    int weight_version;
    int bias_version;
    size_t row = 0;

    if (file == NULL || fgets(line, sizeof(line), file) == NULL)
        return false;
    size_t count = split_csv(line, fields, 64);
    gain_memory = column(fields, count, "f_gain_memory");
    gain_thread = column(fields, count, "f_gain_thread");
    memory_final = column(fields, count, "memory_score_final");
    thread_final = column(fields, count, "thread_score_final");
    margin = column(fields, count, "decision_margin");
    decision = column(fields, count, "decision");
    weight_version = column(fields, count, "weight_version");
    bias_version = column(fields, count, "bias_version");
    if (gain_memory < 0 || gain_thread < 0 || memory_final < 0 || thread_final < 0 || margin < 0 ||
        decision < 0 || weight_version < 0 || bias_version < 0) {
        fclose(file);
        return false;
    }
    while (row < 4 && fgets(line, sizeof(line), file) != NULL) {
        count = split_csv(line, fields, 64);
        if ((size_t)bias_version >= count || (size_t)margin >= count) {
            fclose(file);
            return false;
        }
        rows[row].factor_gain_memory = strtod(fields[gain_memory], NULL);
        rows[row].factor_gain_thread = strtod(fields[gain_thread], NULL);
        rows[row].memory_final = strtod(fields[memory_final], NULL);
        rows[row].thread_final = strtod(fields[thread_final], NULL);
        snprintf(rows[row].margin_text, sizeof(rows[row].margin_text), "%s", fields[margin]);
        rows[row].margin = strcmp(fields[margin], "NA") == 0 ? NAN : strtod(fields[margin], NULL);
        snprintf(rows[row].decision, sizeof(rows[row].decision), "%s", fields[decision]);
        rows[row].weight_version = (unsigned)strtoul(fields[weight_version], NULL, 10);
        rows[row].bias_version = (unsigned)strtoul(fields[bias_version], NULL, 10);
        row++;
    }
    fclose(file);
    return row == 4;
}

static void report(const char *name, bool passed)
{
    printf("%s: %s\n", name, passed ? "PASS" : "FAIL");
}

int main(void)
{
    static const char *header =
        "timestamp,app_id,pid,entity_id,classification,classification_score,f_access,f_threshold,"
        "f_gain_memory,f_cost_memory,f_cpu_memory,f_sharing_memory,f_gain_thread,f_cost_thread,"
        "f_cpu_thread,f_sharing_thread\n";
    char directory[] = "/tmp/decision-benefit-evidence-XXXXXX";
    char input_path[256];
    char output_path[256];
    char state_path[256];
    char history_path[256];
    char log_path[256];
    char cleanup_path[512];
    FILE *input;
    decision_summary_t summary;
    output_row_t rows[4] = {0};
    bool passed;

    if (mkdtemp(directory) == NULL)
        return EXIT_FAILURE;
    snprintf(input_path, sizeof(input_path), "%s/input.csv", directory);
    snprintf(output_path, sizeof(output_path), "%s/output.csv", directory);
    snprintf(state_path, sizeof(state_path), "%s/state", directory);
    snprintf(history_path, sizeof(history_path), "%s/history", directory);
    snprintf(log_path, sizeof(log_path), "%s/decision.log", directory);
    input = fopen(input_path, "w");
    if (input == NULL)
        return EXIT_FAILURE;
    fputs(header, input);
    fputs("2026-09-07T12:00:00Z,decision-app,42,thread-win,HOT,50,0,0,0,0,0,0,0.2,0,0,0\n", input);
    fputs("2026-09-07T12:00:01Z,decision-app,42,epsilon-tie,HOT,50,0,0,0.2,0,0,0,0.18,0,0,0\n", input);
    fputs("2026-09-07T12:00:02Z,decision-app,42,zero-margin,HOT,50,0,0,0,0,0,0,0,0,0,0\n", input);
    fputs("2026-09-07T12:00:03Z,decision-app,42,unavailable,HOT,50,NA,0,0,0,0,0,0,0,0,0\n", input);
    if (fclose(input) != 0)
        return EXIT_FAILURE;
    decision_config_t config = {
        .input_path = input_path, .output_path = output_path, .state_dir = state_path,
        .history_dir = history_path, .log_path = log_path, .epsilon = 0.05,
        .bias_min = -0.25, .bias_max = 0.25, .history_max_records = 100,
        .history_max_days = 30.0, .history_decay_lambda = 0.1, .cleanup_interval = 100,
        .inactive_days = 7.0, .expire_days = 30.0
    };

    passed = decision_run(&config, &summary) == 0 && read_rows(output_path, rows);
    report("DBE01_DECISION_ENGINE_RUNS", passed);
    bool factors_unchanged = passed && equal(rows[0].factor_gain_memory, 0.0) &&
                             equal(rows[0].factor_gain_thread, 0.2);
    report("DBE02_FACTORS_UNCHANGED", factors_unchanged);
    bool thread_utility_and_ranking = passed && equal(rows[0].memory_final, 0.0) &&
                                      equal(rows[0].thread_final, 0.24) &&
                                      equal(rows[0].margin, -0.24) &&
                                      strcmp(rows[0].decision, "MOVE_THREAD") == 0;
    report("DBE03_UTILITY_MARGIN_AND_RANKING_UNCHANGED", thread_utility_and_ranking);
    bool epsilon_unchanged = passed && equal(rows[1].memory_final, 0.24) &&
                             equal(rows[1].thread_final, 0.216) && equal(rows[1].margin, 0.024) &&
                             strcmp(rows[1].decision, "NO_MIGRATION") == 0;
    report("DBE04_EPSILON_BEHAVIOR_UNCHANGED", epsilon_unchanged);
    bool zero_not_unavailable = passed && equal(rows[2].margin, 0.0) &&
                                strcmp(rows[2].margin_text, "NA") != 0 &&
                                strcmp(rows[2].decision, "NO_MIGRATION") == 0;
    report("DBE05_ZERO_MARGIN_IS_AVAILABLE", zero_not_unavailable);
    bool unavailable_remains_unavailable = passed && strcmp(rows[3].margin_text, "NA") == 0 &&
                                         strcmp(rows[3].decision, "INSUFFICIENT_DECISION_SIGNAL") == 0;
    report("DBE06_UNAVAILABLE_MARGIN_REMAINS_UNAVAILABLE", unavailable_remains_unavailable);
    bool versions_preserved = passed && rows[0].weight_version == 1 && rows[0].bias_version == 1 &&
                              summary.move_thread == 1 && summary.no_migration == 2 &&
                              summary.insufficient_rows == 1;
    report("DBE07_VERSION_AND_SUMMARY_UNCHANGED", versions_preserved);
    passed = passed && factors_unchanged && thread_utility_and_ranking && epsilon_unchanged &&
             zero_not_unavailable && unavailable_remains_unavailable && versions_preserved;
    unlink(input_path); unlink(output_path); unlink(log_path);
    snprintf(cleanup_path, sizeof(cleanup_path), "%s/weights.csv", state_path); unlink(cleanup_path);
    snprintf(cleanup_path, sizeof(cleanup_path), "%s/biases.csv", state_path); unlink(cleanup_path);
    snprintf(cleanup_path, sizeof(cleanup_path), "%s/application_state.csv", state_path); unlink(cleanup_path);
    snprintf(cleanup_path, sizeof(cleanup_path), "%s/decisions.csv", history_path); unlink(cleanup_path);
    snprintf(cleanup_path, sizeof(cleanup_path), "%s/weight_history.csv", history_path); unlink(cleanup_path);
    rmdir(state_path); rmdir(history_path); rmdir(directory);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
