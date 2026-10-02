#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "runtime_evidence.h"

#include <ctype.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

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

static bool number(const char *text, double *value)
{
    char *end = NULL;

    if (text == NULL || *text == '\0' || strcmp(text, "NA") == 0 || strcmp(text, "-1") == 0)
        return false;
    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' && isfinite(*value) && *value >= 0.0;
}

static double clamp01(double value)
{
    return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
}

/* Parse the monitor's compact N<num>=<pages> representation without assuming node IDs. */
static int dominant_memory_node(const char *text)
{
    int dominant = -1;
    unsigned long long pages = 0;
    const char *cursor = text;

    while (cursor != NULL && *cursor != '\0') {
        char *end = NULL;
        unsigned long node;
        unsigned long long value;

        while (*cursor != 'N' && *cursor != '\0')
            cursor++;
        if (*cursor == '\0')
            break;
        node = strtoul(cursor + 1, &end, 10);
        if (end == cursor + 1 || *end != '=') {
            cursor++;
            continue;
        }
        value = strtoull(end + 1, &end, 10);
        if (end != NULL && value > pages && node <= INT_MAX) {
            pages = value;
            dominant = (int)node;
        }
        cursor = end;
    }
    return dominant;
}

int runtime_evidence_write_classifier_input(const char *monitor_path, const char *app_id,
                                            const char *output_path)
{
    FILE *input = fopen(monitor_path, "r");
    FILE *output = NULL;
    char *line = NULL, *fields[MAX_FIELDS];
    size_t capacity = 0;
    int cpu, minor, major, references, misses;
    int result = -1;

    if (input == NULL || app_id == NULL || *app_id == '\0')
        goto done;
    output = fopen(output_path, "w");
    if (output == NULL || getline(&line, &capacity, input) < 0)
        goto done;
    size_t count = split_csv(line, fields);
    cpu = column(fields, count, "process_cpu_utilization_percent");
    minor = column(fields, count, "interval_minor_faults");
    major = column(fields, count, "interval_major_faults");
    references = column(fields, count, "cache_references");
    misses = column(fields, count, "cache_misses");
    if (cpu < 0 || minor < 0 || major < 0)
        goto done;
    for (size_t index = 0; index < count; index++)
        fprintf(output, "%s%s", index == 0 ? "" : ",", fields[index]);
    fputs(",app_id,entity_id,access_value,access_status\n", output);
    while (getline(&line, &capacity, input) >= 0) {
        double cpu_value, minor_value, major_value, ref_value, miss_value;
        bool cache_available;
        double access;

        count = split_csv(line, fields);
        if ((size_t)cpu >= count || (size_t)minor >= count || (size_t)major >= count ||
            !number(fields[cpu], &cpu_value) || !number(fields[minor], &minor_value) ||
            !number(fields[major], &major_value))
            continue;
        cache_available = references >= 0 && misses >= 0 && (size_t)references < count &&
                          (size_t)misses < count && number(fields[references], &ref_value) &&
                          number(fields[misses], &miss_value) && ref_value > 0.0 && miss_value <= ref_value;
        /* CPU and fault activity are mandatory measured signals; cache quality refines, never invents, access. */
        access = 100.0 * (0.70 * clamp01(cpu_value / 100.0) +
                          0.30 * clamp01((minor_value + major_value) / 100.0));
        if (cache_available)
            access += 25.0 * clamp01(miss_value / ref_value);
        for (size_t index = 0; index < count; index++)
            fprintf(output, "%s%s", index == 0 ? "" : ",", fields[index]);
        fprintf(output, ",%s,process,%.9f,%s\n", app_id, access,
                cache_available ? "MEASURED_CPU_FAULT_CACHE" : "MEASURED_CPU_FAULT");
    }
    result = ferror(input) || ferror(output) ? -1 : 0;
done:
    free(line);
    if (input != NULL) fclose(input);
    if (output != NULL) fclose(output);
    return result;
}

static int thread_node_for_sample(const char *thread_path, const char *timestamp, const char *pid,
                                  int dominant_memory_node, pid_t *selected_tid)
{
    FILE *file = fopen(thread_path, "r");
    char *line = NULL, *fields[MAX_FIELDS];
    size_t capacity = 0;
    int timestamp_column, pid_column, tid_column, node_column, usage_column, result = -1;
    double highest = -1.0;

    if (file == NULL || getline(&line, &capacity, file) < 0)
        goto done;
    size_t count = split_csv(line, fields);
    timestamp_column = column(fields, count, "timestamp");
    pid_column = column(fields, count, "pid");
    tid_column = column(fields, count, "tid");
    node_column = column(fields, count, "cpu_node");
    usage_column = column(fields, count, "cpu_utilization_percent");
    if (timestamp_column < 0 || pid_column < 0 || tid_column < 0 || node_column < 0 || usage_column < 0)
        goto done;
    while (getline(&line, &capacity, file) >= 0) {
        double usage;
        char *end = NULL;
        long node;
        count = split_csv(line, fields);
        if ((size_t)usage_column >= count || (size_t)timestamp_column >= count || (size_t)pid_column >= count ||
            (size_t)tid_column >= count ||
            (size_t)node_column >= count || strcmp(fields[timestamp_column], timestamp) != 0 ||
            strcmp(fields[pid_column], pid) != 0 || !number(fields[usage_column], &usage))
            continue;
        node = strtol(fields[node_column], &end, 10);
        char *tid_end = NULL;
        long tid = strtol(fields[tid_column], &tid_end, 10);
        if (end != fields[node_column] && *end == '\0' && node >= 0 && node <= INT_MAX &&
            node != dominant_memory_node &&
            tid_end != fields[tid_column] && *tid_end == '\0' && tid > 0 &&
            (usage > highest || (usage == highest && (selected_tid == NULL || *selected_tid <= 0 || tid < *selected_tid)))) {
            highest = usage;
            result = (int)node;
            if (selected_tid != NULL)
                *selected_tid = (pid_t)tid;
        }
    }
done:
    free(line);
    if (file != NULL) fclose(file);
    return result;
}

int runtime_evidence_write_decision_input(const char *evidence_path, const char *thread_path,
                                           const char *classification_path, uint64_t start_time_ticks,
                                           const char *output_path)
{
    FILE *evidence = fopen(evidence_path, "r"), *classification = fopen(classification_path, "r"), *output = NULL;
    char *evidence_line = NULL, *classification_line = NULL, *fields[MAX_FIELDS], *classified[MAX_FIELDS];
    size_t evidence_capacity = 0, classification_capacity = 0;
    int timestamp, elapsed, pid, app, entity, access, memory;
    int class_timestamp, class_pid, class_entity, score, classification_value;
    int result = -1;

    if (evidence == NULL || classification == NULL)
        goto done;
    output = fopen(output_path, "w");
    if (output == NULL || getline(&evidence_line, &evidence_capacity, evidence) < 0 ||
        getline(&classification_line, &classification_capacity, classification) < 0)
        goto done;
    size_t count = split_csv(evidence_line, fields);
    timestamp = column(fields, count, "timestamp"); elapsed = column(fields, count, "elapsed_ms");
    pid = column(fields, count, "pid"); app = column(fields, count, "app_id");
    entity = column(fields, count, "entity_id"); access = column(fields, count, "access_value");
    memory = column(fields, count, "process_numa_pages");
    size_t classified_count = split_csv(classification_line, classified);
    class_timestamp = column(classified, classified_count, "timestamp"); class_pid = column(classified, classified_count, "pid");
    class_entity = column(classified, classified_count, "entity_id"); score = column(classified, classified_count, "score");
    classification_value = column(classified, classified_count, "current_class");
    if (timestamp < 0 || elapsed < 0 || pid < 0 || app < 0 || entity < 0 || access < 0 || memory < 0 ||
        class_timestamp < 0 || class_pid < 0 || class_entity < 0 || score < 0 || classification_value < 0)
        goto done;
    fputs("timestamp,elapsed_ms,pid,start_time_ticks,app_id,entity_id,classification,classification_score,f_access,f_threshold,f_gain_memory,f_cost_memory,f_cpu_memory,f_sharing_memory,f_gain_thread,f_cost_thread,f_cpu_thread,f_sharing_thread,memory_dominant_node,thread_dominant_node,placement_relation,evidence_status\n", output);
    while (getline(&evidence_line, &evidence_capacity, evidence) >= 0 &&
           getline(&classification_line, &classification_capacity, classification) >= 0) {
        double access_value;
        int memory_node, thread_node;
        pid_t selected_tid = -1;
        char selected_entity[32];
        count = split_csv(evidence_line, fields);
        classified_count = split_csv(classification_line, classified);
        if ((size_t)access >= count || (size_t)memory >= count || (size_t)timestamp >= count ||
            (size_t)pid >= count || (size_t)class_timestamp >= classified_count || (size_t)class_pid >= classified_count ||
            strcmp(fields[timestamp], classified[class_timestamp]) != 0 || strcmp(fields[pid], classified[class_pid]) != 0 ||
            !number(fields[access], &access_value))
            continue;
        memory_node = dominant_memory_node(fields[memory]);
        thread_node = thread_node_for_sample(thread_path, fields[timestamp], fields[pid], memory_node,
                                             &selected_tid);
        snprintf(selected_entity, sizeof(selected_entity), "%ld", (long)selected_tid);
        if (memory_node < 0 || thread_node < 0) {
            fprintf(output, "%s,%s,%s,%llu,%s,%s,%s,%s,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,%d,%d,LOCAL,NO_REMOTE_THREAD\n",
                     fields[timestamp], fields[elapsed], fields[pid], (unsigned long long)start_time_ticks, fields[app], selected_entity,
                    classified[classification_value], classified[score], memory_node, thread_node);
        } else {
            double normalized = clamp01(access_value / 125.0);
            fprintf(output, "%s,%s,%s,%llu,%s,%s,%s,%s,%.9f,%.9f,NA,NA,NA,NA,%.9f,%.9f,%.9f,%.9f,%d,%d,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY\n",
                     fields[timestamp], fields[elapsed], fields[pid], (unsigned long long)start_time_ticks, fields[app], selected_entity,
                    classified[classification_value], classified[score], normalized, clamp01(access_value / 100.0),
                    normalized, 0.0, clamp01(access_value / 100.0), 0.0, memory_node, thread_node);
        }
    }
    result = ferror(evidence) || ferror(classification) || ferror(output) ? -1 : 0;
done:
    free(evidence_line); free(classification_line);
    if (evidence != NULL) fclose(evidence);
    if (classification != NULL) fclose(classification);
    if (output != NULL) fclose(output);
    return result;
}
