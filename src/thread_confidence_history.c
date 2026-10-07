#define _POSIX_C_SOURCE 200809L

#include "thread_confidence_history.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY_SCHEMA_VERSION "1"
#define HISTORY_HEADER "schema_version,timestamp_utc,app_id,pid,start_time_ticks,temporal_generation,candidate_tid,candidate_tid_available,candidate_tid_verified,classification,classification_available,placement_relation,placement_available,evidence_status,observation_valid,reason,provenance"
#define HISTORY_FIELDS 17U

typedef struct {
    char **lines;
    size_t count;
    size_t capacity;
} history_lines_t;

static size_t split_csv(char *line, char **fields, size_t capacity)
{
    size_t count = 0;
    char *cursor = line;

    while (cursor != NULL && count < capacity) {
        fields[count++] = cursor;
        cursor = strchr(cursor, ',');
        if (cursor != NULL) *cursor++ = '\0';
    }
    return count;
}

static bool valid_text(const char *text)
{
    return text != NULL && text[0] != '\0' && strpbrk(text, ",\r\n") == NULL;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (text == NULL || text[0] == '\0') return false;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_pid(const char *text, pid_t *value)
{
    uint64_t parsed;

    if (!parse_u64(text, &parsed) || parsed == 0 || parsed > INT32_MAX) return false;
    *value = (pid_t)parsed;
    return true;
}

static bool parse_bool(const char *text, bool *value)
{
    if (strcmp(text, "true") == 0) { *value = true; return true; }
    if (strcmp(text, "false") == 0) { *value = false; return true; }
    return false;
}

static bool valid_status(const char *text)
{
    static const char *names[] = {"VALID", "INVALID_EVIDENCE", "CANDIDATE_UNAVAILABLE", "CANDIDATE_VERIFIED", "MALFORMED_INPUT",
                                  "IDENTITY_MISMATCH", "PERSISTENCE_UNAVAILABLE", "UNSUPPORTED"};
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++)
        if (strcmp(text, names[index]) == 0) return true;
    return false;
}

static bool valid_row(char *line, char **fields)
{
    pid_t pid;
    uint64_t ticks, generation;
    bool candidate_available, candidate_verified, classification_available, placement_available, observation_valid;

    if (split_csv(line, fields, HISTORY_FIELDS + 1) != HISTORY_FIELDS ||
        strcmp(fields[0], HISTORY_SCHEMA_VERSION) != 0 || !valid_text(fields[1]) || !valid_text(fields[2]) ||
        !parse_pid(fields[3], &pid) || !parse_u64(fields[4], &ticks) || ticks == 0 ||
        !parse_u64(fields[5], &generation) || generation == 0 ||
        !parse_bool(fields[7], &candidate_available) || !parse_bool(fields[8], &candidate_verified) ||
        !parse_bool(fields[10], &classification_available) || !parse_bool(fields[12], &placement_available) ||
        !parse_bool(fields[14], &observation_valid) || !valid_status(fields[13]) || !valid_text(fields[15]) ||
        !valid_text(fields[16])) return false;
    if (candidate_available) {
        pid_t candidate;
        if (!candidate_verified || !parse_pid(fields[6], &candidate)) return false;
    } else if (strcmp(fields[6], "NA") != 0 || candidate_verified) return false;
    if ((classification_available && !valid_text(fields[9])) ||
        (placement_available && !valid_text(fields[11])) ||
        (!classification_available && strcmp(fields[9], "NA") != 0) ||
        (!placement_available && strcmp(fields[11], "NA") != 0)) return false;
    return true;
}

static int add_line(history_lines_t *history, const char *line)
{
    char **lines;

    if (history->count == history->capacity) {
        size_t capacity = history->capacity == 0 ? 8 : history->capacity * 2;
        lines = realloc(history->lines, capacity * sizeof(*lines));
        if (lines == NULL) return -1;
        history->lines = lines;
        history->capacity = capacity;
    }
    history->lines[history->count] = strdup(line);
    if (history->lines[history->count] == NULL) return -1;
    history->count++;
    return 0;
}

static void free_history(history_lines_t *history)
{
    for (size_t index = 0; index < history->count; index++) free(history->lines[index]);
    free(history->lines);
}

static int load_history(const char *path, history_lines_t *history)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;

    memset(history, 0, sizeof(*history));
    file = fopen(path, "r");
    if (file == NULL) return errno == ENOENT ? 0 : -1;
    if ((length = getline(&line, &capacity, file)) < 0 ||
        strncmp(line, HISTORY_HEADER, strlen(HISTORY_HEADER)) != 0 ||
        line[strlen(HISTORY_HEADER)] != '\n') goto malformed;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *copy;
        char *fields[HISTORY_FIELDS + 1];

        if (length == 0 || line[length - 1] != '\n') goto malformed;
        line[length - 1] = '\0';
        copy = strdup(line);
        if (copy == NULL || !valid_row(copy, fields) || add_line(history, line) != 0) {
            free(copy);
            goto malformed;
        }
        free(copy);
    }
    free(line);
    if (fclose(file) == 0) return 0;
malformed:
    free(line);
    fclose(file);
    free_history(history);
    return -1;
}

static int write_history(const char *path, const history_lines_t *history)
{
    char temporary[4096];
    int descriptor;
    FILE *file;

    if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary)) return -1;
    descriptor = mkstemp(temporary);
    if (descriptor < 0) return -1;
    file = fdopen(descriptor, "w");
    if (file == NULL) { close(descriptor); unlink(temporary); return -1; }
    if (fprintf(file, "%s\n", HISTORY_HEADER) < 0) goto failed;
    for (size_t index = 0; index < history->count; index++)
        if (fprintf(file, "%s\n", history->lines[index]) < 0) goto failed;
    if (fflush(file) != 0 || fsync(descriptor) != 0 || fclose(file) != 0 || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
failed:
    fclose(file);
    unlink(temporary);
    return -1;
}

const char *thread_confidence_history_status_name(thread_confidence_history_status_t status)
{
    static const char *names[] = {"VALID", "INVALID_EVIDENCE", "CANDIDATE_UNAVAILABLE", "CANDIDATE_VERIFIED", "MALFORMED_INPUT",
                                  "IDENTITY_MISMATCH", "PERSISTENCE_UNAVAILABLE", "UNSUPPORTED"};
    return status >= THREAD_CONFIDENCE_HISTORY_VALID && status <= THREAD_CONFIDENCE_HISTORY_UNSUPPORTED ?
        names[status] : "UNSUPPORTED";
}

static bool same_observation(const char *existing, const char *candidate)
{
    char *existing_copy = strdup(existing);
    char *candidate_copy = strdup(candidate);
    char *existing_fields[HISTORY_FIELDS + 1];
    char *candidate_fields[HISTORY_FIELDS + 1];
    bool same = existing_copy != NULL && candidate_copy != NULL &&
                valid_row(existing_copy, existing_fields) && valid_row(candidate_copy, candidate_fields);

    for (size_t index = 0; same && index < HISTORY_FIELDS; index++)
        if (index != 1 && strcmp(existing_fields[index], candidate_fields[index]) != 0) same = false;
    free(existing_copy);
    free(candidate_copy);
    return same;
}

int thread_confidence_history_next_generation(const char *path, const char *app_id, pid_t pid,
                                              uint64_t start_time_ticks, uint64_t *generation)
{
    history_lines_t history;
    uint64_t highest = 0;

    if (path == NULL || !valid_text(app_id) || pid <= 0 || start_time_ticks == 0 || generation == NULL ||
        load_history(path, &history) != 0) return -1;
    for (size_t index = 0; index < history.count; index++) {
        char *copy = strdup(history.lines[index]);
        char *fields[HISTORY_FIELDS + 1];
        uint64_t row_pid, row_ticks, row_generation;

        if (copy == NULL) { free_history(&history); return -1; }
        (void)valid_row(copy, fields);
        if (parse_u64(fields[3], &row_pid) && parse_u64(fields[4], &row_ticks) &&
            parse_u64(fields[5], &row_generation) && strcmp(fields[2], app_id) == 0 &&
            row_pid == (uint64_t)pid && row_ticks == start_time_ticks && row_generation > highest)
            highest = row_generation;
        free(copy);
    }
    free_history(&history);
    if (highest == UINT64_MAX) return -1;
    *generation = highest + 1;
    return 0;
}

int thread_confidence_history_persist(const char *path,
                                      const thread_confidence_history_observation_t *observation)
{
    history_lines_t history;
    char candidate[32], row[2048];
    int written;

    if (path == NULL || observation == NULL || !valid_text(observation->timestamp_utc) ||
        !valid_text(observation->app_id) || observation->pid <= 0 || observation->start_time_ticks == 0 ||
        observation->temporal_generation == 0 || !valid_text(observation->reason) ||
        !valid_text(observation->provenance) || !valid_text(thread_confidence_history_status_name(observation->evidence_status)) ||
        observation->evidence_status < THREAD_CONFIDENCE_HISTORY_VALID ||
        observation->evidence_status > THREAD_CONFIDENCE_HISTORY_UNSUPPORTED ||
        observation->observation_valid || observation->evidence_status == THREAD_CONFIDENCE_HISTORY_VALID ||
        (observation->candidate_tid_available && observation->evidence_status != THREAD_CONFIDENCE_HISTORY_CANDIDATE_VERIFIED) ||
        (!observation->candidate_tid_available && observation->evidence_status == THREAD_CONFIDENCE_HISTORY_CANDIDATE_VERIFIED) ||
        (observation->candidate_tid_available && (!observation->candidate_tid_verified || observation->candidate_tid <= 0)) ||
        (!observation->candidate_tid_available && (observation->candidate_tid != -1 || observation->candidate_tid_verified)) ||
        (observation->classification_available && !valid_text(observation->classification)) ||
        (observation->placement_available && !valid_text(observation->placement_relation))) return -1;
    if (load_history(path, &history) != 0) return -1;
    snprintf(candidate, sizeof(candidate), "%s", observation->candidate_tid_available ? "0" : "NA");
    if (observation->candidate_tid_available)
        snprintf(candidate, sizeof(candidate), "%ld", (long)observation->candidate_tid);
    written = snprintf(row, sizeof(row), "%s,%s,%s,%ld,%llu,%llu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s",
                       HISTORY_SCHEMA_VERSION, observation->timestamp_utc, observation->app_id, (long)observation->pid,
                       (unsigned long long)observation->start_time_ticks,
                       (unsigned long long)observation->temporal_generation, candidate,
                       observation->candidate_tid_available ? "true" : "false",
                       observation->candidate_tid_verified ? "true" : "false",
                       observation->classification_available ? observation->classification : "NA",
                       observation->classification_available ? "true" : "false",
                       observation->placement_available ? observation->placement_relation : "NA",
                       observation->placement_available ? "true" : "false",
                       thread_confidence_history_status_name(observation->evidence_status),
                       observation->observation_valid ? "true" : "false", observation->reason,
                       observation->provenance);
    if (written < 0 || (size_t)written >= sizeof(row)) { free_history(&history); return -1; }
    for (size_t index = 0; index < history.count; index++) {
        char *copy = strdup(history.lines[index]);
        char *fields[HISTORY_FIELDS + 1];

        if (copy == NULL) { free_history(&history); return -1; }
        (void)valid_row(copy, fields);
        if (strcmp(fields[2], observation->app_id) == 0 && strtol(fields[3], NULL, 10) == observation->pid &&
            strtoull(fields[4], NULL, 10) == observation->start_time_ticks &&
            strtoull(fields[5], NULL, 10) == observation->temporal_generation) {
            int same = same_observation(history.lines[index], row);
            free(copy);
            free_history(&history);
            return same ? 0 : -1;
        }
        free(copy);
    }
    if (add_line(&history, row) != 0) { free_history(&history); return -1; }
    int result = write_history(path, &history);
    free_history(&history);
    return result;
}
