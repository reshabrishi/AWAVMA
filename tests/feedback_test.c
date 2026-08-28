#include "feedback.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char directory_template[] = "/tmp/awavma-feedback-test-XXXXXX";
static char state_dir[1024];
static char history_dir[1024];
static char history_path[1024];
static char results_path[1024];
static char log_path[1024];
static FeedbackConfig config;
static int failures;

static void report_test(const char *name, const char *expected, const char *actual, bool pass)
{
    printf("%s | %s | %s | %s\n", name, expected, actual, pass ? "PASS" : "FAIL");
    if (!pass)
        failures++;
}

static FeedbackConfig make_config(void)
{
    return (FeedbackConfig){
        .state_dir = state_dir,
        .history_dir = history_dir,
        .results_path = results_path,
        .history_path = history_path,
        .log_path = log_path,
        .learning_rate = 0.05,
        .bias_learning_rate = 0.05,
        .min_samples = 5,
        .deadband = 0.05,
        .reward_min = -1.0,
        .reward_max = 1.0,
        .decay_lambda = 0.1,
        .min_confidence = 0.5,
        .history_max_records = 1000,
        .history_max_days = 30.0,
        .cleanup_interval = 100,
        .min_weight = 0.0,
        .max_weight = 2.0,
        .min_bias = -0.25,
        .max_bias = 0.25,
        .throughput_weight = 0.4,
        .execution_time_weight = 0.2,
        .latency_weight = 0.2,
        .page_fault_weight = 0.2
    };
}

static void remove_state(void)
{
    char path[1024];

    Feedback_Shutdown();
    unlink(history_path);
    unlink(results_path);
    unlink(log_path);
    snprintf(path, sizeof(path), "%s/weights.csv", state_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/biases.csv", state_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/application_state.csv", state_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/weights.csv.tmp", state_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/biases.csv.tmp", state_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/feedback_history.csv.tmp", history_dir); unlink(path);
    config = make_config();
    if (!Feedback_Init(&config))
        failures++;
}

static FeedbackEvent event_for(const char *app_id, const char *id,
                               ValidationAction action, bool positive)
{
    FeedbackEvent event;

    memset(&event, 0, sizeof(event));
    snprintf(event.timestamp, sizeof(event.timestamp), "2026-08-18T00:00:00Z");
    snprintf(event.feedback_id, sizeof(event.feedback_id), "%s-feedback", id);
    snprintf(event.migration_id, sizeof(event.migration_id), "%s", id);
    snprintf(event.app_id, sizeof(event.app_id), "%s", app_id);
    snprintf(event.entity_id, sizeof(event.entity_id), "workload");
    event.pid = 1234;
    event.action = action;
    snprintf(event.phase6_validation, sizeof(event.phase6_validation), "APPROVED");
    snprintf(event.migration_result, sizeof(event.migration_result), "MIGRATION_SUCCESS");
    event.recorded_at_epoch = (long long)time(NULL);
    event.epoch_available = true;
    event.before_samples = 10;
    event.after_samples = 10;
    event.sample_counts_available = true;
    event.supplied_confidence = 0.9;
    event.supplied_confidence_available = true;
    event.before_metrics[FEEDBACK_METRIC_THROUGHPUT] = positive ? 100.0 : 110.0;
    event.after_metrics[FEEDBACK_METRIC_THROUGHPUT] = positive ? 110.0 : 100.0;
    event.before_metrics[FEEDBACK_METRIC_EXECUTION_TIME] = positive ? 10.0 : 9.0;
    event.after_metrics[FEEDBACK_METRIC_EXECUTION_TIME] = positive ? 9.0 : 10.0;
    event.before_metrics[FEEDBACK_METRIC_LATENCY] = positive ? 10.0 : 9.0;
    event.after_metrics[FEEDBACK_METRIC_LATENCY] = positive ? 9.0 : 10.0;
    event.before_metrics[FEEDBACK_METRIC_PAGE_FAULTS] = positive ? 100.0 : 90.0;
    event.after_metrics[FEEDBACK_METRIC_PAGE_FAULTS] = positive ? 90.0 : 100.0;
    for (size_t index = 0; index < FEEDBACK_METRIC_COUNT; index++)
        event.metric_available[index] = true;
    for (size_t index = 0; index < FEEDBACK_SIGNAL_COUNT; index++) {
        event.factor_available[index] = true;
        event.factor_signals[index] = 0.5;
    }
    event.migration_execution_time_ms = 0.05;
    event.migration_cost_available = true;
    event.pages_migrated = 1;
    return event;
}

static bool read_bias(const char *app_id, double *memory, double *thread)
{
    char path[1024];
    FILE *file;
    char line[512];

    snprintf(path, sizeof(path), "%s/biases.csv", state_dir);
    file = fopen(path, "r");
    if (file == NULL)
        return false;
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char id[128];
        double memory_value;
        double thread_value;

        if (sscanf(line, "%127[^,],%lf,%lf", id, &memory_value, &thread_value) == 3 && strcmp(id, app_id) == 0) {
            *memory = memory_value;
            *thread = thread_value;
            fclose(file);
            return true;
        }
    }
    fclose(file);
    return false;
}

static bool read_weight(const char *app_id, const char *action, const char *factor, double *value)
{
    char path[1024];
    FILE *file;
    char line[512];

    snprintf(path, sizeof(path), "%s/weights.csv", state_dir);
    file = fopen(path, "r");
    if (file == NULL)
        return false;
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char id[128], row_action[32], row_factor[32];
        double row_value;

        if (sscanf(line, "%127[^,],%31[^,],%31[^,],%lf", id, row_action, row_factor, &row_value) == 4 &&
            strcmp(id, app_id) == 0 && strcmp(row_action, action) == 0 && strcmp(row_factor, factor) == 0) {
            *value = row_value;
            fclose(file);
            return true;
        }
    }
    fclose(file);
    return false;
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

static bool process_event(FeedbackEvent *event, FeedbackResult *result, FeedbackUpdateStatus expected)
{
    FeedbackUpdateStatus actual = ProcessFeedback(event, result);

    return actual == expected;
}

static void write_application_state(const char *app_id, const char *status)
{
    char path[1024];
    FILE *file;

    snprintf(path, sizeof(path), "%s/application_state.csv", state_dir);
    file = fopen(path, "w");
    fprintf(file, "app_id,pid,entity_count,hot_count,moderate_count,cold_count,current_profile_version,last_seen,status\n");
    fprintf(file, "%s,1234,1,0,0,0,1,2020-01-01T00:00:00Z,%s\n", app_id, status);
    fclose(file);
}

int main(void)
{
    char *directory = mkdtemp(directory_template);
    FeedbackEvent event;
    FeedbackResult result;
    double memory_bias, thread_bias, other_bias;
    double old_weight, new_weight;
    double first_bias, second_bias;
    double first_weight, second_weight;
    time_t now = time(NULL);

    if (directory == NULL)
        return EXIT_FAILURE;
    snprintf(state_dir, sizeof(state_dir), "%s/state", directory);
    snprintf(history_dir, sizeof(history_dir), "%s/history", directory);
    snprintf(history_path, sizeof(history_path), "%s/feedback_history.csv", history_dir);
    snprintf(results_path, sizeof(results_path), "%s/feedback_results.csv", history_dir);
    snprintf(log_path, sizeof(log_path), "%s/feedback.log", directory);
    mkdir(state_dir, 0700);
    mkdir(history_dir, 0700);
    remove_state();

    event = event_for("APP_A", "F01", VALIDATION_ACTION_MOVE_MEMORY, true);
    bool f01 = process_event(&event, &result, FEEDBACK_UPDATED) && read_bias("APP_A", &memory_bias, &thread_bias) &&
               memory_bias > 0.0 && read_weight("APP_A", "MEMORY", "ACCESS", &old_weight) &&
               fabs(result.state.new_memory_weights[FACTOR_ACCESS] - old_weight) < 1e-12;
    report_test("F01", "positive reward and memory update", FeedbackStatusName(result.update_status), f01);

    event = event_for("APP_NEG", "F02", VALIDATION_ACTION_MOVE_MEMORY, false);
    bool f02 = process_event(&event, &result, FEEDBACK_UPDATED) && read_bias("APP_NEG", &memory_bias, &thread_bias) && memory_bias < 0.0;
    report_test("F02", "negative reward and memory decrease", FeedbackStatusName(result.update_status), f02);

    event = event_for("APP_THREAD", "F03", VALIDATION_ACTION_MOVE_THREAD, true);
    bool f03 = process_event(&event, &result, FEEDBACK_UPDATED) && read_bias("APP_THREAD", &memory_bias, &thread_bias) && thread_bias > 0.0;
    report_test("F03", "positive thread update", FeedbackStatusName(result.update_status), f03);

    event = event_for("APP_THREAD_NEG", "F04", VALIDATION_ACTION_MOVE_THREAD, false);
    bool f04 = process_event(&event, &result, FEEDBACK_UPDATED) && read_bias("APP_THREAD_NEG", &memory_bias, &thread_bias) && thread_bias < 0.0;
    report_test("F04", "negative thread update", FeedbackStatusName(result.update_status), f04);

    event = event_for("APP_ISO_A", "F05A", VALIDATION_ACTION_MOVE_MEMORY, true);
    process_event(&event, &result, FEEDBACK_UPDATED);
    event = event_for("APP_ISO_B", "F05B", VALIDATION_ACTION_MOVE_MEMORY, false);
    process_event(&event, &result, FEEDBACK_UPDATED);
    read_bias("APP_ISO_A", &first_bias, &thread_bias);
    read_bias("APP_ISO_B", &second_bias, &other_bias);
    report_test("F05", "opposite applications diverge", "APP_A up / APP_B down", first_bias > 0.0 && second_bias < 0.0);

    decision_feedback_update_t zero_update = {0};
    decision_feedback_snapshot_t unused_snapshot;
    decision_config_t decision_config = {.state_dir = state_dir, .history_dir = history_dir, .bias_min = -0.25, .bias_max = 0.25};
    decision_feedback_apply(&decision_config, "APP_ISO_CHECK", &zero_update, 0.0, 2.0, &unused_snapshot);
    read_bias("APP_ISO_CHECK", &first_bias, &thread_bias);
    event = event_for("APP_ISO_A", "F06", VALIDATION_ACTION_MOVE_MEMORY, true);
    process_event(&event, &result, FEEDBACK_UPDATED);
    read_bias("APP_ISO_CHECK", &second_bias, &other_bias);
    report_test("F06", "APP_B state unchanged by APP_A", "unchanged", fabs(first_bias - second_bias) < 1e-12);

    event = event_for("APP_WEIGHTS", "F07M", VALIDATION_ACTION_MOVE_MEMORY, true);
    process_event(&event, &result, FEEDBACK_UPDATED);
    bool all_weight_deltas = true;
    for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++)
        all_weight_deltas = all_weight_deltas && fabs((result.state.new_memory_weights[factor] - result.state.old_memory_weights[factor]) - result.applied_update.memory_delta[factor]) < 1e-12;
    event = event_for("APP_WEIGHTS", "F07T", VALIDATION_ACTION_MOVE_THREAD, true);
    process_event(&event, &result, FEEDBACK_UPDATED);
    for (int factor = 0; factor < (int)DECISION_FACTOR_COUNT; factor++)
        all_weight_deltas = all_weight_deltas && fabs((result.state.new_thread_weights[factor] - result.state.old_thread_weights[factor]) - result.applied_update.thread_delta[factor]) < 1e-12;
    report_test("F07", "all 12 individual factor deltas explainable", "memory 6 + thread 6 match", all_weight_deltas);
    report_test("F08", "only relevant bias changes", "thread bias delta matches", fabs((result.state.new_thread_bias - result.state.old_thread_bias) - result.applied_update.thread_bias_delta) < 1e-12 && fabs(result.applied_update.memory_bias_delta) < 1e-12);

    Feedback_Shutdown();
    config = make_config(); config.learning_rate = 100.0; config.max_weight = 2.0; Feedback_Init(&config);
    event = event_for("APP_BOUNDS", "F09", VALIDATION_ACTION_MOVE_MEMORY, true);
    bool bounds_updates = true;
    for (int index = 0; index < 20; index++)
        if (ProcessFeedback(&event, &result) != FEEDBACK_UPDATED)
            bounds_updates = false;
    read_weight("APP_BOUNDS", "MEMORY", "ACCESS", &new_weight);
    report_test("F09", "weight remains within [min_weight,max_weight]", "bounded", bounds_updates && isfinite(new_weight) && new_weight >= -1e-12 && new_weight <= 2.0 + 1e-12);
    Feedback_Shutdown();
    config = make_config(); config.min_bias = -0.05; config.max_bias = 0.05; Feedback_Init(&config);
    event = event_for("APP_BIAS_BOUNDS", "F10", VALIDATION_ACTION_MOVE_MEMORY, true);
    for (int index = 0; index < 20; index++) ProcessFeedback(&event, &result);
    read_bias("APP_BIAS_BOUNDS", &memory_bias, &thread_bias);
    report_test("F10", "bias remains inside bounds", "bounded", memory_bias <= 0.05 + 1e-12 && memory_bias >= -0.05 - 1e-12);
    config = make_config(); Feedback_Shutdown(); Feedback_Init(&config);

    event = event_for("APP_DEADBAND", "F11", VALIDATION_ACTION_MOVE_MEMORY, true);
    event.after_metrics[FEEDBACK_METRIC_THROUGHPUT] = event.before_metrics[FEEDBACK_METRIC_THROUGHPUT];
    event.after_metrics[FEEDBACK_METRIC_EXECUTION_TIME] = event.before_metrics[FEEDBACK_METRIC_EXECUTION_TIME];
    event.after_metrics[FEEDBACK_METRIC_LATENCY] = event.before_metrics[FEEDBACK_METRIC_LATENCY];
    event.after_metrics[FEEDBACK_METRIC_PAGE_FAULTS] = event.before_metrics[FEEDBACK_METRIC_PAGE_FAULTS];
    bool f11 = process_event(&event, &result, FEEDBACK_UPDATE_NO_UPDATE);
    report_test("F11", "FEEDBACK_NO_UPDATE inside deadband", FeedbackStatusName(result.update_status), f11);

    event = event_for("APP_SAMPLES", "F12", VALIDATION_ACTION_MOVE_MEMORY, true);
    event.before_samples = 1; event.after_samples = 1;
    bool f12 = process_event(&event, &result, FEEDBACK_UPDATE_INSUFFICIENT_OBSERVATION);
    report_test("F12", "insufficient observations", FeedbackStatusName(result.update_status), f12);

    event = event_for("APP_CONF", "F13", VALIDATION_ACTION_MOVE_MEMORY, true);
    event.supplied_confidence = 0.1;
    bool f13 = process_event(&event, &result, FEEDBACK_UPDATE_NO_UPDATE);
    report_test("F13", "low confidence produces no update", FeedbackStatusName(result.update_status), f13);

    event = event_for("APP_INVALID", "F14", VALIDATION_ACTION_MOVE_MEMORY, true);
    event.before_metrics[FEEDBACK_METRIC_THROUGHPUT] = NAN;
    bool f14 = process_event(&event, &result, FEEDBACK_UPDATE_INVALID_INPUT);
    event.before_metrics[FEEDBACK_METRIC_THROUGHPUT] = INFINITY;
    f14 = f14 && process_event(&event, &result, FEEDBACK_UPDATE_INVALID_INPUT);
    report_test("F14", "NaN/infinity metric rejected", FeedbackStatusName(result.update_status), f14);

    event = event_for("APP_UNAVAILABLE", "F15", VALIDATION_ACTION_MOVE_MEMORY, true);
    event.metric_available[FEEDBACK_METRIC_PAGE_FAULTS] = false;
    event.before_metrics[FEEDBACK_METRIC_PAGE_FAULTS] = -1.0;
    event.after_metrics[FEEDBACK_METRIC_PAGE_FAULTS] = -1.0;
    bool f15 = process_event(&event, &result, FEEDBACK_UPDATED);
    report_test("F15", "unavailable metric excluded", FeedbackStatusName(result.update_status), f15 && result.metrics_used == 3);

    event = event_for("APP_FAIL", "F16", VALIDATION_ACTION_MOVE_MEMORY, true);
    snprintf(event.migration_result, sizeof(event.migration_result), "MIGRATION_PERMISSION_DENIED");
    bool f16 = process_event(&event, &result, FEEDBACK_UPDATE_NOT_LEARNABLE);
    report_test("F16", "execution failure not learnable", FeedbackStatusName(result.update_status), f16);
    snprintf(event.migration_result, sizeof(event.migration_result), "MIGRATION_TARGET_GONE");
    bool f17 = process_event(&event, &result, FEEDBACK_UPDATE_NOT_LEARNABLE);
    report_test("F17", "target gone not learnable", FeedbackStatusName(result.update_status), f17);

    event = event_for("APP_HARM", "F18", VALIDATION_ACTION_MOVE_MEMORY, false);
    bool f18 = process_event(&event, &result, FEEDBACK_UPDATED) && read_bias("APP_HARM", &memory_bias, &thread_bias) && memory_bias < 0.0;
    report_test("F18", "successful harmful migration is negative", FeedbackClassName(result.feedback_class), f18);
    event = event_for("APP_BENEFIT", "F19", VALIDATION_ACTION_MOVE_MEMORY, true);
    bool f19 = process_event(&event, &result, FEEDBACK_UPDATED) && read_bias("APP_BENEFIT", &memory_bias, &thread_bias) && memory_bias > 0.0;
    report_test("F19", "successful beneficial migration is positive", FeedbackClassName(result.feedback_class), f19);
    event = event_for("APP_NEUTRAL", "F20", VALIDATION_ACTION_MOVE_MEMORY, true);
    for (int metric = 0; metric < (int)FEEDBACK_METRIC_COUNT; metric++) event.after_metrics[metric] = event.before_metrics[metric];
    bool f20 = process_event(&event, &result, FEEDBACK_UPDATE_NO_UPDATE);
    report_test("F20", "neutral outcome has no update", FeedbackStatusName(result.update_status), f20);

    event = event_for("APP_RECENT", "F21R", VALIDATION_ACTION_MOVE_MEMORY, true);
    event.recorded_at_epoch = (long long)now;
    process_event(&event, &result, FEEDBACK_UPDATED);
    read_bias("APP_RECENT", &first_bias, &thread_bias);
    event = event_for("APP_OLD", "F21O", VALIDATION_ACTION_MOVE_MEMORY, true);
    event.recorded_at_epoch = (long long)now - 30 * 86400;
    process_event(&event, &result, FEEDBACK_UPDATED);
    read_bias("APP_OLD", &second_bias, &thread_bias);
    report_test("F21", "recent feedback has greater influence", "recent delta > old delta", first_bias > second_bias);

    event = event_for("APP_OSC", "F22", VALIDATION_ACTION_MOVE_MEMORY, true);
    for (int index = 0; index < 20; index++) {
        bool positive = (index % 2) == 0;
        event = event_for("APP_OSC", "F22", VALIDATION_ACTION_MOVE_MEMORY, positive);
        ProcessFeedback(&event, &result);
    }
    read_weight("APP_OSC", "MEMORY", "ACCESS", &new_weight);
    report_test("F22", "opposite sequence remains bounded", "finite bounded weight", isfinite(new_weight) && new_weight >= 0.0 && new_weight <= 2.0);

    write_application_state("APP_EXPIRED", "EXPIRED");
    event = event_for("APP_EXPIRED", "F23", VALIDATION_ACTION_MOVE_MEMORY, true);
    bool f23 = process_event(&event, &result, FEEDBACK_UPDATE_NO_UPDATE);
    report_test("F23", "expired application is not updated", FeedbackStatusName(result.update_status), f23);

    event = event_for("APP_PERSIST", "F24", VALIDATION_ACTION_MOVE_MEMORY, true);
    process_event(&event, &result, FEEDBACK_UPDATED);
    read_bias("APP_PERSIST", &first_bias, &thread_bias);
    Feedback_Shutdown();
    config = make_config(); Feedback_Init(&config);
    read_bias("APP_PERSIST", &second_bias, &thread_bias);
    report_test("F24", "state survives restart", "bias preserved", fabs(first_bias - second_bias) < 1e-12);

    Feedback_Shutdown();
    {
        char path[1024];
        FILE *file;

        snprintf(path, sizeof(path), "%s/weights.csv", state_dir);
        file = fopen(path, "w");
        fputs("corrupt\n", file);
        fclose(file);
    }
    config = make_config(); Feedback_Init(&config);
    event = event_for("APP_CORRUPT", "F25", VALIDATION_ACTION_MOVE_MEMORY, true);
    bool f25 = process_event(&event, &result, FEEDBACK_UPDATE_INVALID_STATE);
    report_test("F25", "invalid state preserved and rejected", FeedbackStatusName(result.update_status), f25);
    remove_state();

    Feedback_Shutdown();
    config = make_config(); config.history_max_records = 3; config.cleanup_interval = 1; Feedback_Init(&config);
    for (int index = 0; index < 5; index++) {
        event = event_for("APP_HISTORY", "F26", VALIDATION_ACTION_MOVE_MEMORY, true);
        snprintf(event.migration_result, sizeof(event.migration_result), "MIGRATION_PERMISSION_DENIED");
        ProcessFeedback(&event, &result);
    }
    report_test("F26", "3 feedback records retained", "header plus 3 records", line_count(history_path) == 4);

    {
        char temporary[1024];
        FILE *file = fopen(history_path, "w");

        fputs("header\nrecord,1.0,1\n", file);
        fclose(file);
        snprintf(temporary, sizeof(temporary), "%s.tmp", history_path);
        mkdir(temporary, 0700);
        report_test("F27", "original history preserved on compaction failure", "original retained", !Feedback_CleanupHistory() && line_count(history_path) == 2);
        rmdir(temporary);
    }

    remove_state();
    event = event_for("APP_DETERMINISTIC", "F28", VALIDATION_ACTION_MOVE_MEMORY, true);
    ProcessFeedback(&event, &result);
    read_bias("APP_DETERMINISTIC", &first_bias, &thread_bias);
    read_weight("APP_DETERMINISTIC", "MEMORY", "ACCESS", &first_weight);
    remove_state();
    event = event_for("APP_DETERMINISTIC", "F28", VALIDATION_ACTION_MOVE_MEMORY, true);
    ProcessFeedback(&event, &result);
    read_bias("APP_DETERMINISTIC", &second_bias, &thread_bias);
    read_weight("APP_DETERMINISTIC", "MEMORY", "ACCESS", &second_weight);
    report_test("F28", "identical event/state is deterministic", "identical reward and state", fabs(first_bias - second_bias) < 1e-12 && fabs(first_weight - second_weight) < 1e-12);

    FeedbackUpdateStatus integration_status;
    event = event_for("APP_INT_MEM", "I01", VALIDATION_ACTION_MOVE_MEMORY, true);
    integration_status = ProcessFeedback(&event, &result);
    report_test("I01", "approved beneficial memory integration", FeedbackStatusName(integration_status), integration_status == FEEDBACK_UPDATED);
    event = event_for("APP_INT_MEM_NEG", "I02", VALIDATION_ACTION_MOVE_MEMORY, false);
    integration_status = ProcessFeedback(&event, &result);
    report_test("I02", "approved harmful memory integration", FeedbackStatusName(integration_status), integration_status == FEEDBACK_UPDATED);
    event = event_for("APP_INT_THREAD", "I03", VALIDATION_ACTION_MOVE_THREAD, true);
    integration_status = ProcessFeedback(&event, &result);
    report_test("I03", "approved beneficial thread integration", FeedbackStatusName(integration_status), integration_status == FEEDBACK_UPDATED);
    event = event_for("APP_INT_THREAD_NEG", "I04", VALIDATION_ACTION_MOVE_THREAD, false);
    integration_status = ProcessFeedback(&event, &result);
    report_test("I04", "approved harmful thread integration", FeedbackStatusName(integration_status), integration_status == FEEDBACK_UPDATED);
    event = event_for("APP_REAL", "I05", VALIDATION_ACTION_INSUFFICIENT, false);
    snprintf(event.phase6_validation, sizeof(event.phase6_validation), "REJECTED");
    snprintf(event.migration_result, sizeof(event.migration_result), "MIGRATION_INSUFFICIENT_INFORMATION");
    integration_status = ProcessFeedback(&event, &result);
    report_test("I05", "real insufficient pipeline produces no learning", FeedbackStatusName(integration_status), integration_status == FEEDBACK_UPDATE_NO_UPDATE);

    remove_state();
    for (int index = 0; index < 12; index++) {
        event = event_for("APP_A", "ADAPT_A", VALIDATION_ACTION_MOVE_MEMORY, true);
        if (ProcessFeedback(&event, &result) != FEEDBACK_UPDATED)
            failures++;
        event = event_for("APP_B", "ADAPT_B", VALIDATION_ACTION_MOVE_MEMORY, false);
        if (ProcessFeedback(&event, &result) != FEEDBACK_UPDATED)
            failures++;
    }
    read_bias("APP_A", &first_bias, &thread_bias);
    read_bias("APP_B", &second_bias, &other_bias);
    read_weight("APP_A", "MEMORY", "ACCESS", &first_weight);
    read_weight("APP_B", "MEMORY", "ACCESS", &second_weight);
    report_test("ADAPT", "APP_A trends up and APP_B trends down", "opposite isolated trends", first_bias > second_bias && first_weight > second_weight);

    event = event_for("APP_STABLE", "STABLE", VALIDATION_ACTION_MOVE_MEMORY, true);
    for (int index = 0; index < 200; index++)
        ProcessFeedback(&event, &result);
    read_bias("APP_STABLE", &first_bias, &thread_bias);
    read_weight("APP_STABLE", "MEMORY", "ACCESS", &first_weight);
    event = event_for("APP_STABLE", "STABLE_NEG", VALIDATION_ACTION_MOVE_MEMORY, false);
    for (int index = 0; index < 200; index++)
        ProcessFeedback(&event, &result);
    read_bias("APP_STABLE", &second_bias, &thread_bias);
    read_weight("APP_STABLE", "MEMORY", "ACCESS", &second_weight);
    report_test("STABILITY", "finite bounded opposite movement", "positive then negative bounded", isfinite(first_bias) && isfinite(second_bias) && isfinite(first_weight) && isfinite(second_weight) && first_bias >= -0.25 && first_bias <= 0.25 && second_bias >= -0.25 && second_bias <= 0.25 && second_weight >= 0.0 && second_weight <= 2.0 && second_bias < first_bias);

    double learning_deltas[3] = {0.0, 0.0, 0.0};
    const double learning_rates[3] = {0.01, 0.05, 0.10};
    for (int index = 0; index < 3; index++) {
        Feedback_Shutdown();
        config = make_config();
        config.learning_rate = learning_rates[index];
        config.bias_learning_rate = learning_rates[index];
        Feedback_Init(&config);
        event = event_for(index == 0 ? "APP_LR_01" : index == 1 ? "APP_LR_05" : "APP_LR_10",
                          "LR", VALIDATION_ACTION_MOVE_MEMORY, true);
        ProcessFeedback(&event, &result);
        read_bias(event.app_id, &learning_deltas[index], &thread_bias);
    }
    report_test("LEARNING_RATE", "0.01 < 0.05 < 0.10 update magnitude", "monotonic update magnitude", learning_deltas[0] < learning_deltas[1] && learning_deltas[1] < learning_deltas[2]);

    Feedback_Shutdown();
    unlink(history_path); unlink(results_path); unlink(log_path);
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/weights.csv", state_dir); unlink(path);
        snprintf(path, sizeof(path), "%s/biases.csv", state_dir); unlink(path);
        snprintf(path, sizeof(path), "%s/application_state.csv", state_dir); unlink(path);
    }
    rmdir(state_dir); rmdir(history_dir); rmdir(directory);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
