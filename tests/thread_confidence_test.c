#define _POSIX_C_SOURCE 200809L

#include "thread_confidence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *header =
    "timestamp,elapsed_ms,pid,start_time_ticks,app_id,entity_id,classification,classification_score,placement_relation,evidence_status\n";

static int write_cycle(const char *path, const char *rows)
{
    FILE *file = fopen(path, "w");
    int result = file != NULL && fputs(header, file) >= 0 && fputs(rows, file) >= 0 && fclose(file) == 0;
    return result;
}

static int evaluate_rows(const char *rows, pid_t tid, int valid, size_t count, const char *reason,
                         ThreadConfidence *confidence)
{
    char path[] = "/tmp/thread-confidence-XXXXXX";
    int descriptor = mkstemp(path);
    int result;

    if (descriptor < 0) return 0;
    close(descriptor);
    result = write_cycle(path, rows) &&
        thread_confidence_evaluate(path, 42, 99, tid, confidence) == valid &&
        confidence->valid_sample_count == count && strcmp(confidence->reason, reason) == 0;
    unlink(path);
    return result;
}

static void report(const char *name, int passed, int *suite)
{
    printf("%s: %s\n", name, passed ? "PASS" : "FAIL");
    *suite = *suite && passed;
}

static int read_contents(const char *path, char *buffer, size_t size)
{
    FILE *file = fopen(path, "r");
    size_t count;
    if (file == NULL) return 0;
    count = fread(buffer, 1, size - 1, file);
    fclose(file);
    buffer[count] = '\0';
    return 1;
}

int main(void)
{
    const char *hot = "t,1,42,99,a,101,HOT,1,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY\n";
    const char *hot100 = "t,1,42,100,a,101,HOT,1,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY\n";
    char two[512], three[768], five[1280], cold[768], local[768], tid_change[768], generation[768];
    ThreadConfidence confidence;
    int suite = 1, passed;

    snprintf(two, sizeof(two), "%s%s", hot, hot);
    snprintf(three, sizeof(three), "%s%s%s", hot, hot, hot);
    snprintf(five, sizeof(five), "%s%s%s%s%s", hot, hot, hot, hot, hot);
    snprintf(cold, sizeof(cold), "%s%s%st,1,42,99,a,101,COLD,1,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY\n", hot, hot, hot);
    snprintf(local, sizeof(local), "%s%s%st,1,42,99,a,101,HOT,1,LOCAL,MEASURED_LOCAL_PLACEMENT\n", hot, hot, hot);
    snprintf(tid_change, sizeof(tid_change), "%s%st,1,42,99,a,102,HOT,1,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY\n", hot, hot);
    snprintf(generation, sizeof(generation), "%s%st,1,42,100,a,101,HOT,1,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY\n", hot, hot);

    report("CF01_LATEST_THREE_HOT_REMOTE_SAME_TID", evaluate_rows(three, 101, 1, 3, "VALID", &confidence), &suite);
    report("CF02_OLD_STREAK_NOT_ACCEPTED", evaluate_rows(cold, 101, 0, 0, "CLASS_CHANGED", &confidence), &suite);
    report("CF03_ONLY_TWO_SAMPLES", evaluate_rows(two, 101, 0, 2, "INSUFFICIENT_SAMPLES", &confidence), &suite);
    report("CF04_LOCAL_BREAKS_STREAK", evaluate_rows(local, 101, 0, 0, "PLACEMENT_NOT_REMOTE", &confidence), &suite);
    report("CF05_TID_CHANGE_BREAKS_STREAK", evaluate_rows(tid_change, 102, 0, 1, "INSUFFICIENT_SAMPLES", &confidence), &suite);
    report("CF06_GENERATION_CHANGE", evaluate_rows(generation, 101, 0, 0, "GENERATION_CHANGED", &confidence), &suite);
    report("CF07_MALFORMED_REQUIRED_FIELD", evaluate_rows("broken\n", 101, 0, 0, "EVIDENCE_UNAVAILABLE", &confidence), &suite);
    report("CF08_MORE_THAN_THREE", evaluate_rows(five, 101, 1, 5, "VALID", &confidence), &suite);
    passed = evaluate_rows(cold, 101, 0, 0, "CLASS_CHANGED", &confidence) &&
        strcmp(confidence.latest_classification, "COLD") == 0 &&
        strcmp(confidence.latest_placement_relation, "REMOTE") == 0;
    report("CF09_DIAGNOSTIC_STRING_LIFETIME", passed, &suite);

    {
        char cycle99[] = "/tmp/thread-cycle99-XXXXXX", cycle100[] = "/tmp/thread-cycle100-XXXXXX";
        char history[] = "/tmp/thread-history-XXXXXX";
        int first = mkstemp(cycle99), second = mkstemp(cycle100), third = mkstemp(history);
        close(first); close(second); close(third);
        passed = write_cycle(cycle99, hot) && write_cycle(cycle100, hot100) &&
            thread_confidence_append(cycle99, history, 42, 99, 1) &&
            thread_confidence_append(cycle99, history, 42, 99, 1) &&
            !thread_confidence_evaluate(history, 42, 99, 101, &confidence) && confidence.valid_sample_count == 1 &&
            thread_confidence_append(cycle99, history, 42, 99, 2) &&
            !thread_confidence_evaluate(history, 42, 99, 101, &confidence) && confidence.valid_sample_count == 2 &&
            thread_confidence_append(cycle99, history, 42, 99, 3) &&
            thread_confidence_evaluate(history, 42, 99, 101, &confidence) && confidence.valid_sample_count == 3 &&
            thread_confidence_append(cycle100, history, 42, 100, 1) &&
            !thread_confidence_evaluate(history, 42, 100, 101, &confidence) && confidence.valid_sample_count == 1 &&
            strcmp(confidence.reason, "INSUFFICIENT_SAMPLES") == 0;
        unlink(cycle99); unlink(cycle100); unlink(history);
    }
    report("CF10_DUPLICATE_CYCLE_DOES_NOT_INFLATE_CONFIDENCE", passed, &suite);

    {
        char cycle[] = "/tmp/thread-long-XXXXXX", history[] = "/tmp/thread-long-history-XXXXXX";
        char long_row[1024]; int first = mkstemp(cycle), second = mkstemp(history);
        close(first); close(second);
        snprintf(long_row, sizeof(long_row), "t,1,42,99,%0250d,101,HOT,1,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY\n", 1);
        passed = write_cycle(cycle, long_row) && thread_confidence_append(cycle, history, 42, 99, 1) &&
            thread_confidence_append(cycle, history, 42, 99, 1) &&
            !thread_confidence_evaluate(history, 42, 99, 101, &confidence) && confidence.valid_sample_count == 1;
        unlink(cycle); unlink(history);
    }
    report("CF11_LONG_HISTORY_ROW_AND_HEADER", passed, &suite);

    {
        char cycle[] = "/tmp/thread-mismatch-XXXXXX", history[] = "/tmp/thread-mismatch-history-XXXXXX";
        char before[4096], after[4096]; int first = mkstemp(cycle), second = mkstemp(history);
        close(first); close(second);
        passed = write_cycle(cycle, hot) && thread_confidence_append(cycle, history, 42, 99, 1) &&
            read_contents(history, before, sizeof(before)) &&
            write_cycle(cycle, "t,1,43,99,a,101,HOT,1,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY\n") &&
            !thread_confidence_append(cycle, history, 42, 99, 2) &&
            read_contents(history, after, sizeof(after)) && strcmp(before, after) == 0;
        unlink(cycle); unlink(history);
    }
    report("CF12_CYCLE_IDENTITY_MISMATCH_FAILS_CLOSED", passed, &suite);
    return suite ? EXIT_SUCCESS : EXIT_FAILURE;
}
