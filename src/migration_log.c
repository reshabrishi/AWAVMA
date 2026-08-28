#include "migration_log.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RESULTS_HEADER "timestamp,migration_id,app_id,pid,expected_start_time_ticks,tid,entity_id,action,phase5_decision,phase6_validation,source_node,destination_node,destination_cpu,requested,attempted,successful,failed,execution_time_ms,verification_status,result,error_reason"
#define HISTORY_HEADER "timestamp,migration_id,app_id,pid,expected_start_time_ticks,tid,entity_id,action,phase5_decision,phase6_validation,source_node,destination_node,destination_cpu,requested,attempted,successful,failed,execution_time_ms,verification_status,result,error_reason,history_relevance,recorded_at_epoch"
#define STATE_HEADER "migration_id,app_id,pid,tid,entity_id,action,status,updated_at_epoch"

static MigrationConfig active_config;
static bool initialized;
static size_t report_count;

static long long epoch_seconds(void)
{
    return (long long)time(NULL);
}

static int append_line(const char *path, const char *header, const char *line)
{
    FILE *file = fopen(path, "a+");
    long position;

    if (file == NULL)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    position = ftell(file);
    if (position < 0) {
        fclose(file);
        return -1;
    }
    if (position == 0 && fprintf(file, "%s\n", header) < 0) {
        fclose(file);
        return -1;
    }
    if (fprintf(file, "%s\n", line) < 0 || fflush(file) != 0) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static int write_state(const MigrationRequest *request, const MigrationReport *report)
{
    char temporary[512] = {0};
    FILE *file;
    long long now = epoch_seconds();

    if (snprintf(temporary, sizeof(temporary), "%s.tmp", active_config.state_path) >= (int)sizeof(temporary))
        return -1;
    file = fopen(temporary, "w");
    if (file == NULL)
        return -1;
    if (fprintf(file, "%s\n%s,%s,%ld,%ld,%s,%s,%s,%lld\n", STATE_HEADER,
                report->migration_id, report->app_id, (long)report->pid, (long)report->tid,
                report->entity_id, MigrationActionName(request->phase5_decision.action),
                MigrationResultName(report->result), now) < 0 || fflush(file) != 0) {
        fclose(file);
        unlink(temporary);
        return -1;
    }
    if (fclose(file) != 0) {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, active_config.state_path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int compact_history(void)
{
    FILE *input = NULL;
    FILE *output = NULL;
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t length;
    char *header = NULL;
    char **records = NULL;
    size_t count = 0;
    long long now = epoch_seconds();
    long long max_age = (long long)(active_config.history_max_days * 86400.0);
    char temporary[512] = {0};

    input = fopen(active_config.history_path, "r");
    if (input == NULL && errno == ENOENT)
        return 0;
    if (input == NULL)
        return -1;
    length = getline(&line, &line_capacity, input);
    if (length < 0) {
        fclose(input);
        free(line);
        return -1;
    }
    header = strdup(line);
    if (header == NULL)
        goto fail;
    while ((length = getline(&line, &line_capacity, input)) >= 0) {
        char *last_comma = strrchr(line, ',');
        long long recorded = last_comma != NULL ? strtoll(last_comma + 1, NULL, 10) : now;
        char *copy;
        char **expanded;

        if (recorded > 0 && now >= recorded && now - recorded > max_age)
            continue;
        copy = strdup(line);
        if (copy == NULL)
            goto fail;
        expanded = realloc(records, (count + 1) * sizeof(*records));
        if (expanded == NULL) {
            free(copy);
            goto fail;
        }
        records = expanded;
        records[count++] = copy;
    }
    fclose(input);
    input = NULL;
    if (count > active_config.history_max_records) {
        size_t first = count - active_config.history_max_records;
        size_t index;

        for (index = 0; index < first; index++)
            free(records[index]);
        memmove(records, records + first, active_config.history_max_records * sizeof(*records));
        count = active_config.history_max_records;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", active_config.history_path) >= (int)sizeof(temporary))
        goto fail;
    output = fopen(temporary, "w");
    if (output == NULL)
        goto fail;
    if (fputs(header, output) == EOF)
        goto fail_output;
    for (size_t index = 0; index < count; index++) {
        char *last_comma = strrchr(records[index], ',');
        char *previous_comma = last_comma != NULL ? last_comma - 1 : NULL;
        long long recorded = last_comma != NULL ? strtoll(last_comma + 1, NULL, 10) : now;
        double age_days = recorded > 0 && now >= recorded ? (double)(now - recorded) / 86400.0 : 0.0;

        while (previous_comma != NULL && previous_comma > records[index] && *previous_comma != ',')
            previous_comma--;
        if (previous_comma == NULL || *previous_comma != ',') {
            if (fputs(records[index], output) == EOF)
                goto fail_output;
        } else {
            if (fwrite(records[index], 1, (size_t)(previous_comma - records[index] + 1), output) == 0 ||
                fprintf(output, "%.9f%s", exp(-active_config.history_decay_lambda * age_days), last_comma) < 0)
                goto fail_output;
        }
    }
    if (fflush(output) != 0 || fclose(output) != 0) {
        output = NULL;
        goto fail;
    }
    output = NULL;
    if (rename(temporary, active_config.history_path) != 0)
        goto fail;
    for (size_t index = 0; index < count; index++)
        free(records[index]);
    free(records);
    free(header);
    free(line);
    return 0;

fail_output:
    fclose(output);
    output = NULL;
    unlink(temporary);
fail:
    if (input != NULL)
        fclose(input);
    if (output != NULL)
        fclose(output);
    unlink(temporary);
    for (size_t index = 0; index < count; index++)
        free(records[index]);
    free(records);
    free(header);
    free(line);
    return -1;
}

bool migration_log_init(const MigrationConfig *config)
{
    if (config == NULL || config->results_path == NULL || config->history_path == NULL ||
        config->log_path == NULL || config->state_path == NULL || config->history_max_records == 0 ||
        config->history_max_days <= 0.0 || !isfinite(config->history_decay_lambda) ||
        config->history_decay_lambda <= 0.0 || config->cleanup_interval == 0)
        return false;
    active_config = *config;
    initialized = true;
    report_count = 0;
    return true;
}

bool migration_log_report(const MigrationRequest *request, const MigrationReport *report)
{
    char line[2048];
    char history_line[2200];
    char human_line[1024];
    FILE *human;
    long long now;

    if (!initialized || request == NULL || report == NULL)
        return false;
    snprintf(line, sizeof(line), "%s,%s,%s,%ld,%llu,%ld,%s,%s,%s,%s,%d,%d,%d,%zu,%zu,%zu,%zu,%.6f,%s,%s,%s",
              report->timestamp, report->migration_id, report->app_id, (long)report->pid,
              (unsigned long long)report->expected_start_time_ticks, (long)report->tid,
              report->entity_id, MigrationActionName(report->action),
             report->phase5_decision, report->phase6_validation, report->source_numa_node,
             report->destination_numa_node, report->destination_cpu, report->pages_requested,
             report->pages_attempted, report->pages_migrated, report->pages_failed,
             report->execution_time_ms, report->verification_status,
             MigrationResultName(report->result), report->error_reason);
    now = epoch_seconds();
    snprintf(history_line, sizeof(history_line), "%s,1.000000000,%lld", line, now);
    if (append_line(active_config.results_path, RESULTS_HEADER, line) != 0 ||
        append_line(active_config.history_path, HISTORY_HEADER, history_line) != 0 ||
        write_state(request, report) != 0)
        return false;
    human = fopen(active_config.log_path, "a");
    if (human == NULL)
        return false;
    snprintf(human_line, sizeof(human_line),
             "%lld migration_id=%s app_id=%s entity=%s pid=%ld tid=%ld action=%s source=%d destination=%d result=%s verification=%s reason=%s\n",
             now, report->migration_id, report->app_id, report->entity_id, (long)report->pid, (long)report->tid,
             MigrationActionName(report->action), report->source_numa_node,
             report->destination_numa_node, MigrationResultName(report->result),
             report->verification_status, report->error_reason);
    if (fputs(human_line, human) == EOF || fclose(human) != 0)
        return false;
    report_count++;
    if (report_count % active_config.cleanup_interval == 0)
        compact_history();
    return true;
}

bool migration_log_cleanup(void)
{
    return initialized && compact_history() == 0;
}

void migration_log_shutdown(void)
{
    initialized = false;
    memset(&active_config, 0, sizeof(active_config));
    report_count = 0;
}
