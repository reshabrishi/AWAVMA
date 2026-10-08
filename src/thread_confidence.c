#define _POSIX_C_SOURCE 200809L
#include "thread_confidence.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HISTORY_HEADER "schema_version,timestamp_utc,app_id,pid,start_time_ticks,temporal_generation,candidate_tid,candidate_tid_available,candidate_tid_verified,classification,classification_available,placement_relation,placement_available,evidence_status,observation_valid,reason,provenance"
#define FIELDS 17U

typedef struct { uint64_t generation; bool qualifying; char semantic[256]; } row_t;

static const char *names[] = {"PASS", "INSUFFICIENT_STREAK", "INSUFFICIENT_EVIDENCE", "CANDIDATE_UNAVAILABLE", "GENERATION_GAP", "INVALID_EVIDENCE", "IDENTITY_MISMATCH", "HISTORY_UNAVAILABLE", "MALFORMED_HISTORY", "UNSUPPORTED_SCHEMA", "DUPLICATE_CONFLICT"};

const char *thread_confidence_status_name(thread_confidence_status_t status)
{
    return status >= THREAD_CONFIDENCE_PASS && status <= THREAD_CONFIDENCE_DUPLICATE_CONFLICT ? names[status] : "MALFORMED_HISTORY";
}

static void set_result(thread_confidence_result_t *result, thread_confidence_status_t status,
                       const char *reason)
{
    result->status = status;
    result->available = status != THREAD_CONFIDENCE_HISTORY_UNAVAILABLE &&
                        status != THREAD_CONFIDENCE_MALFORMED_HISTORY &&
                        status != THREAD_CONFIDENCE_UNSUPPORTED_SCHEMA;
    result->qualified = status == THREAD_CONFIDENCE_PASS;
    snprintf(result->reason, sizeof(result->reason), "%s", reason);
}

static size_t split(char *line, char **fields)
{
    size_t count = 0;
    while (line != NULL && count < FIELDS + 1) { fields[count++] = line; line = strchr(line, ','); if (line != NULL) *line++ = '\0'; }
    return count;
}

static bool u64(const char *text, uint64_t *value)
{
    char *end; unsigned long long parsed;
    if (text == NULL || *text == '\0' || *text == '-') return false;
    errno = 0; parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = parsed; return true;
}

static bool boolean(const char *text, bool *value)
{
    if (strcmp(text, "true") == 0) { *value = true; return true; }
    if (strcmp(text, "false") == 0) { *value = false; return true; }
    return false;
}

static bool supported_classification(const char *value)
{
    return strcmp(value, "HOT") == 0 || strcmp(value, "MODERATE") == 0 || strcmp(value, "COLD") == 0;
}

static bool supported_placement(const char *value)
{
    return strcmp(value, "LOCAL") == 0 || strcmp(value, "REMOTE") == 0;
}

int thread_confidence_evaluate(const char *path, const char *app_id, pid_t pid,
                               uint64_t ticks, pid_t tid, thread_confidence_result_t *result)
{
    FILE *file; char *line = NULL, *fields[FIELDS + 1]; size_t capacity = 0, count = 0, allocated = 0;
    row_t *rows = NULL; bool other_identity = false, invalid = false; ssize_t length;
    if (result == NULL || app_id == NULL || *app_id == '\0' || pid <= 0 || ticks == 0) return -1;
    memset(result, 0, sizeof(*result)); snprintf(result->app_id, sizeof(result->app_id), "%s", app_id);
    result->pid = pid; result->start_time_ticks = ticks; result->candidate_tid = tid;
    result->required_streak = THREAD_CONFIDENCE_REQUIRED_STREAK;
    if (tid <= 0) { set_result(result, THREAD_CONFIDENCE_CANDIDATE_UNAVAILABLE, "candidate_tid_unavailable"); return 0; }
    file = fopen(path, "r");
    if (file == NULL) { set_result(result, errno == ENOENT ? THREAD_CONFIDENCE_HISTORY_UNAVAILABLE : THREAD_CONFIDENCE_HISTORY_UNAVAILABLE, "history_unavailable"); return 0; }
    if ((length = getline(&line, &capacity, file)) < 0) goto malformed;
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, HISTORY_HEADER) != 0) { set_result(result, THREAD_CONFIDENCE_UNSUPPORTED_SCHEMA, "unsupported_schema"); goto done; }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        uint64_t row_pid, row_ticks, generation, row_tid; bool available, verified, class_available, placement_available, valid;
        bool same_process, same_candidate, qualifying;
        line[strcspn(line, "\r\n")] = '\0';
        if (split(line, fields) != FIELDS || strcmp(fields[0], "1") != 0 || !u64(fields[3], &row_pid) ||
            !u64(fields[4], &row_ticks) || !u64(fields[5], &generation) || generation == 0 ||
            !boolean(fields[7], &available) || !boolean(fields[8], &verified) ||
            !boolean(fields[10], &class_available) || !boolean(fields[12], &placement_available) ||
            !boolean(fields[14], &valid)) goto malformed;
        same_process = strcmp(fields[2], app_id) == 0 && row_pid == (uint64_t)pid && row_ticks == ticks;
        if (!same_process) continue;
        if (!available || !u64(fields[6], &row_tid)) { invalid = true; continue; }
        same_candidate = row_tid == (uint64_t)tid;
        if (!same_candidate) { other_identity = true; continue; }
        qualifying = verified && valid && class_available && placement_available &&
                     strcmp(fields[13], "VALID") == 0 && supported_classification(fields[9]) &&
                     supported_placement(fields[11]);
        if (count == allocated) { size_t next = allocated == 0 ? 8 : allocated * 2; row_t *copy = realloc(rows, next * sizeof(*rows)); if (copy == NULL) goto malformed; rows = copy; allocated = next; }
        snprintf(rows[count].semantic, sizeof(rows[count].semantic), "%s|%s|%s|%s|%s|%s|%s", fields[14], fields[9], fields[10], fields[11], fields[12], fields[13], fields[8]);
        rows[count].generation = generation; rows[count++].qualifying = qualifying;
    }
    for (size_t i = 0; i < count; i++) for (size_t j = i + 1; j < count; j++)
        if (rows[i].generation == rows[j].generation && strcmp(rows[i].semantic, rows[j].semantic) != 0) { set_result(result, THREAD_CONFIDENCE_DUPLICATE_CONFLICT, "duplicate_conflict"); goto done; }
    for (size_t i = 0; i < count; i++) for (size_t j = i + 1; j < count; j++) if (rows[j].generation > rows[i].generation) { row_t swap = rows[i]; rows[i] = rows[j]; rows[j] = swap; }
    if (count == 0) { set_result(result, other_identity ? THREAD_CONFIDENCE_IDENTITY_MISMATCH : (invalid ? THREAD_CONFIDENCE_INSUFFICIENT_EVIDENCE : THREAD_CONFIDENCE_INSUFFICIENT_EVIDENCE), other_identity ? "candidate_identity_mismatch" : "no_qualifying_evidence"); goto done; }
    result->latest_temporal_generation = rows[0].generation;
    if (!rows[0].qualifying) { set_result(result, THREAD_CONFIDENCE_INSUFFICIENT_EVIDENCE, "latest_evidence_not_qualifying"); goto done; }
    result->streak_length = 1;
    for (size_t i = 1; i < count && rows[i - 1].generation > 1 && rows[i].generation == rows[i - 1].generation - 1 && rows[i].qualifying; i++) result->streak_length++;
    if (result->streak_length >= result->required_streak) set_result(result, THREAD_CONFIDENCE_PASS, "required_consecutive_streak_met");
    else set_result(result, count > result->streak_length ? THREAD_CONFIDENCE_GENERATION_GAP : THREAD_CONFIDENCE_INSUFFICIENT_STREAK, "latest_consecutive_streak_insufficient");
    goto done;
malformed:
    set_result(result, THREAD_CONFIDENCE_MALFORMED_HISTORY, "malformed_history");
done:
    free(rows); free(line); fclose(file); return 0;
}

int thread_confidence_write_state(const char *path, const thread_confidence_result_t *result)
{
    char temporary[4096]; int descriptor; FILE *file;
    if (path == NULL || result == NULL || snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary)) return -1;
    descriptor = mkstemp(temporary); if (descriptor < 0 || (file = fdopen(descriptor, "w")) == NULL) { if (descriptor >= 0) close(descriptor); return -1; }
    fprintf(file, "schema_version,app_id,pid,start_time_ticks,candidate_tid,latest_temporal_generation,streak_length,required_streak,status,available,qualified,reason\n1,%s,%ld,%llu,%ld,%llu,%llu,%llu,%s,%s,%s,%s\n", result->app_id, (long)result->pid, (unsigned long long)result->start_time_ticks, (long)result->candidate_tid, (unsigned long long)result->latest_temporal_generation, (unsigned long long)result->streak_length, (unsigned long long)result->required_streak, thread_confidence_status_name(result->status), result->available ? "true" : "false", result->qualified ? "true" : "false", result->reason);
    if (fflush(file) != 0 || fsync(descriptor) != 0 || fclose(file) != 0 || rename(temporary, path) != 0) { unlink(temporary); return -1; }
    return 0;
}
