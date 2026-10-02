#define _POSIX_C_SOURCE 200809L

#include "thread_confidence.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 64

static size_t split_csv(char *line, char **fields)
{
    size_t count = 0;
    char *cursor = line;
    while (cursor != NULL && count < MAX_FIELDS) {
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

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end;
    if (text == NULL || *text == '\0' || *text == '-') return false;
    errno = 0; *value = strtoull(text, &end, 10);
    return errno == 0 && end != text && *end == '\0';
}

static bool parse_positive_long(const char *text, long *value)
{
    char *end;
    if (text == NULL || *text == '\0') return false;
    errno = 0; *value = strtol(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' && *value > 0;
}

static bool required_columns(char **fields, size_t count, int *pid, int *start, int *tid,
                             int *classification, int *placement, int *status)
{
    *pid = column(fields, count, "pid"); *start = column(fields, count, "start_time_ticks");
    *tid = column(fields, count, "entity_id"); *classification = column(fields, count, "classification");
    *placement = column(fields, count, "placement_relation"); *status = column(fields, count, "evidence_status");
    return *pid >= 0 && *start >= 0 && *tid >= 0 && *classification >= 0 && *placement >= 0 && *status >= 0;
}

bool thread_confidence_append(const char *cycle_path, const char *history_path, long pid,
                              uint64_t start_ticks, uint64_t generation)
{
    FILE *cycle = NULL, *history = NULL;
    char *line = NULL, *fields[MAX_FIELDS];
    size_t capacity = 0, count;
    int pid_col, start_col, tid_col, class_col, placement_col, status_col;

    if (cycle_path == NULL || history_path == NULL || pid <= 0 || start_ticks == 0)
        return false;
    cycle = fopen(cycle_path, "r");
    if (cycle == NULL || getline(&line, &capacity, cycle) < 0)
        goto fail;
    count = split_csv(line, fields);
    if (!required_columns(fields, count, &pid_col, &start_col, &tid_col, &class_col, &placement_col, &status_col))
        goto fail;
    while (getline(&line, &capacity, cycle) >= 0) {
        long row_pid, row_tid; uint64_t row_start;
        count = split_csv(line, fields);
        if ((size_t)pid_col >= count || (size_t)start_col >= count || (size_t)tid_col >= count ||
            (size_t)class_col >= count || (size_t)placement_col >= count || (size_t)status_col >= count ||
            !parse_positive_long(fields[pid_col], &row_pid) || !parse_u64(fields[start_col], &row_start) ||
            !parse_positive_long(fields[tid_col], &row_tid) || row_pid != pid || row_start != start_ticks)
            goto fail;
    }
    if (ferror(cycle)) goto fail;
    rewind(cycle);
    if (getline(&line, &capacity, cycle) < 0) goto fail;
    history = fopen(history_path, "a+");
    if (history == NULL || fseek(history, 0, SEEK_END) != 0) goto fail;
    if (ftell(history) > 0) {
        int generation_col, history_pid_col, history_start_col;
        if (fseek(history, 0, SEEK_SET) != 0 || getline(&line, &capacity, history) < 0) goto fail;
        count = split_csv(line, fields);
        generation_col = column(fields, count, "cycle_generation");
        history_pid_col = column(fields, count, "history_pid");
        history_start_col = column(fields, count, "history_start_time_ticks");
        if (generation_col < 0 || history_pid_col < 0 || history_start_col < 0) goto fail;
        while (getline(&line, &capacity, history) >= 0) {
            long recorded_pid; uint64_t recorded_generation, recorded_start;
            count = split_csv(line, fields);
            if ((size_t)generation_col >= count || (size_t)history_pid_col >= count || (size_t)history_start_col >= count ||
                !parse_u64(fields[generation_col], &recorded_generation) ||
                !parse_positive_long(fields[history_pid_col], &recorded_pid) ||
                !parse_u64(fields[history_start_col], &recorded_start)) goto fail;
            if (recorded_generation == generation && recorded_pid == pid && recorded_start == start_ticks) {
                fclose(cycle); fclose(history); free(line); return true;
            }
        }
        if (ferror(history) || fseek(history, 0, SEEK_END) != 0) goto fail;
    } else if (fprintf(history, "cycle_generation,history_pid,history_start_time_ticks,%s", line) < 0) goto fail;
    while (getline(&line, &capacity, cycle) >= 0)
        if (fprintf(history, "%llu,%ld,%llu,%s", (unsigned long long)generation, pid,
                    (unsigned long long)start_ticks, line) < 0) goto fail;
    fclose(cycle); fclose(history); free(line); return true;
fail:
    if (cycle != NULL) fclose(cycle);
    if (history != NULL) fclose(history);
    free(line);
    return false;
}

bool thread_confidence_evaluate(const char *path, long pid, uint64_t start_ticks, pid_t tid,
                                ThreadConfidence *confidence)
{
    FILE *file = NULL; char *line = NULL, *fields[MAX_FIELDS]; size_t capacity = 0, count;
    int pid_col, start_col, tid_col, class_col, placement_col, status_col;
    if (confidence == NULL) return false;
    memset(confidence, 0, sizeof(*confidence));
    snprintf(confidence->latest_classification, sizeof(confidence->latest_classification), "NA");
    snprintf(confidence->latest_placement_relation, sizeof(confidence->latest_placement_relation), "NA");
    snprintf(confidence->reason, sizeof(confidence->reason), "EVIDENCE_UNAVAILABLE");
    file = path == NULL ? NULL : fopen(path, "r");
    if (file == NULL || getline(&line, &capacity, file) < 0) goto done;
    count = split_csv(line, fields);
    if (!required_columns(fields, count, &pid_col, &start_col, &tid_col, &class_col, &placement_col, &status_col)) goto done;
    while (getline(&line, &capacity, file) >= 0) {
        long row_pid, row_tid; uint64_t row_start; bool hot, remote;
        count = split_csv(line, fields);
        if ((size_t)pid_col >= count || !parse_positive_long(fields[pid_col], &row_pid)) {
            confidence->valid_sample_count = confidence->consecutive_hot_count = confidence->consecutive_remote_count = 0;
            snprintf(confidence->reason, sizeof(confidence->reason), "EVIDENCE_UNAVAILABLE"); continue;
        }
        if (row_pid != pid) continue;
        if ((size_t)start_col >= count || (size_t)tid_col >= count || (size_t)class_col >= count ||
            (size_t)placement_col >= count || (size_t)status_col >= count || !parse_u64(fields[start_col], &row_start) ||
            !parse_positive_long(fields[tid_col], &row_tid)) { confidence->valid_sample_count = confidence->consecutive_hot_count = confidence->consecutive_remote_count = 0; snprintf(confidence->reason,sizeof(confidence->reason),"EVIDENCE_UNAVAILABLE"); continue; }
        snprintf(confidence->latest_classification, sizeof(confidence->latest_classification), "%s", fields[class_col]);
        snprintf(confidence->latest_placement_relation, sizeof(confidence->latest_placement_relation), "%s", fields[placement_col]);
        if (row_start != start_ticks) { confidence->valid_sample_count = confidence->consecutive_hot_count = confidence->consecutive_remote_count = 0; snprintf(confidence->reason,sizeof(confidence->reason),"GENERATION_CHANGED"); continue; }
        if (row_tid != (long)tid) { confidence->valid_sample_count = confidence->consecutive_hot_count = confidence->consecutive_remote_count = 0; snprintf(confidence->reason,sizeof(confidence->reason),"TID_CHANGED"); continue; }
        hot = strcmp(fields[class_col], "HOT") == 0;
        remote = strcmp(fields[placement_col], "REMOTE") == 0 && strcmp(fields[status_col], "MEASURED_REMOTE_THREAD_TO_MEMORY") == 0;
        confidence->consecutive_hot_count = hot ? confidence->consecutive_hot_count + 1 : 0;
        confidence->consecutive_remote_count = remote ? confidence->consecutive_remote_count + 1 : 0;
        confidence->valid_sample_count = hot && remote ? confidence->valid_sample_count + 1 : 0;
        snprintf(confidence->reason, sizeof(confidence->reason), !hot ? "CLASS_CHANGED" : !remote ? "PLACEMENT_NOT_REMOTE" : confidence->valid_sample_count >= 3 ? "VALID" : "INSUFFICIENT_SAMPLES");
    }
done:
    if (file != NULL) fclose(file); free(line);
    return confidence->valid_sample_count >= 3;
}
