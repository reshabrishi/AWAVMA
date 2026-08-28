#include "monitor.h"
#include "monitor_profile.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#if defined(__has_include)
#if __has_include(<numa.h>)
#include <numa.h>
#define AWAVMA_HAVE_NUMA_HEADER 1
#endif
#endif
#ifndef AWAVMA_HAVE_NUMA_HEADER
static int numa_available(void) { return -1; }
static int numa_num_configured_nodes(void) { return -1; }
static int numa_node_of_cpu(int cpu) { (void)cpu; return -1; }
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROC_LINE_SIZE 4096
#define NUMA_SUMMARY_SIZE 256
#define UNAVAILABLE_INT64 (-1LL)

typedef struct {
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
    uint64_t iowait;
    uint64_t irq;
    uint64_t softirq;
    uint64_t steal;
} system_cpu_stats_t;

typedef struct {
    char state;
    uint64_t minor_faults;
    uint64_t major_faults;
    uint64_t user_ticks;
    uint64_t system_ticks;
    uint64_t start_time_ticks;
    uint64_t virtual_bytes;
    int64_t resident_pages;
    int cpu;
} process_stats_t;

typedef struct {
    pid_t tid;
    uint64_t user_ticks;
    uint64_t system_ticks;
} thread_history_t;

typedef struct {
    pid_t tid;
    int cpu;
    int cpu_node;
    char state;
    double cpu_utilization;
} thread_sample_t;

typedef struct {
    int references_fd;
    int misses_fd;
    bool references_available;
    bool misses_available;
    char references_reason[128];
    char misses_reason[128];
} perf_counters_t;

typedef struct {
    process_stats_t process;
    system_cpu_stats_t system;
    uint64_t elapsed_ms;
    double system_cpu_utilization;
    double process_cpu_utilization;
    int64_t rss_kb;
    int64_t vms_kb;
    int64_t rss_delta_kb;
    int64_t interval_minor_faults;
    int64_t interval_major_faults;
    long long cache_references;
    long long cache_misses;
    size_t thread_count;
    char numa_summary[NUMA_SUMMARY_SIZE];
    thread_sample_t *threads;
} monitor_sample_t;

typedef struct {
    pid_t pid;
    unsigned interval_ms;
    long ticks_per_second;
    int cpu_count;
    int numa_nodes;
    bool numa_available;
    FILE *output;
    FILE *thread_output;
    FILE *log;
    perf_counters_t counters;
    bool have_previous_process;
    bool have_previous_system;
    process_stats_t previous_process;
    system_cpu_stats_t previous_system;
    int64_t previous_rss_kb;
    thread_history_t *history;
    size_t history_count;
    double start_monotonic;
    uint64_t sample_count;
    const monitor_config_t *config;
} monitor_state_t;

static const char *monitor_csv_header(void)
{
    return "timestamp,elapsed_ms,pid,system_cpu_utilization_percent,process_cpu_utilization_percent,rss_kb,vms_kb,rss_delta_kb,minor_page_faults,major_page_faults,interval_minor_faults,interval_major_faults,cache_references,cache_misses,thread_count,numa_nodes,process_numa_pages,benchmark_pattern,benchmark_threads,benchmark_memory_mb,benchmark_iterations,benchmark_duration_sec";
}

static const char *thread_csv_header(void)
{
    return "timestamp,elapsed_ms,pid,tid,cpu,cpu_node,cpu_utilization_percent,state";
}

static double monotonic_seconds(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void log_line(monitor_state_t *state, const char *message)
{
    monitor_profile_scope_t profile;

    if (state->log != NULL) {
        monitor_profile_scope_begin(&profile, "sample", "logging", NULL, state->pid, 0);
        fprintf(state->log, "%.3f %s\n", monotonic_seconds() - state->start_monotonic, message);
        fflush(state->log);
        monitor_profile_scope_end(&profile, "OK");
    }
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static uint64_t total_cpu_ticks(const system_cpu_stats_t *stats)
{
    return stats->user + stats->nice + stats->system + stats->idle + stats->iowait +
           stats->irq + stats->softirq + stats->steal;
}

static uint64_t busy_cpu_ticks(const system_cpu_stats_t *stats)
{
    return stats->user + stats->nice + stats->system + stats->irq + stats->softirq + stats->steal;
}

static int read_system_cpu(system_cpu_stats_t *stats)
{
    monitor_profile_scope_t profile;
    FILE *file = fopen("/proc/stat", "r");
    char line[PROC_LINE_SIZE];
    int result;

    monitor_profile_scope_begin(&profile, "sample", "cpu_statistics", NULL, -1, 0);

    if (file == NULL) {
        monitor_profile_scope_end(&profile, "ERROR");
        return -1;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        monitor_profile_scope_end(&profile, "ERROR");
        return -1;
    }
    fclose(file);
    result = sscanf(line, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                    " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
                    &stats->user, &stats->nice, &stats->system, &stats->idle,
                    &stats->iowait, &stats->irq, &stats->softirq, &stats->steal) == 8 ? 0 : -1;
    monitor_profile_scope_end(&profile, result == 0 ? "OK" : "ERROR");
    return result;
}

/* /proc/[pid]/stat has a parenthesized command name, so fields are tokenized after its final ')'. */
static int parse_proc_stat_line(const char *line, process_stats_t *stats)
{
    const char *closing = strrchr(line, ')');
    char fields[PROC_LINE_SIZE];
    char *token;
    char *save = NULL;
    unsigned field = 0;

    if (closing == NULL || strlen(closing + 1) >= sizeof(fields))
        return -1;
    strcpy(fields, closing + 1);
    token = strtok_r(fields, " ", &save);
    while (token != NULL) {
        uint64_t value;

        if (field == 0) {
            stats->state = token[0];
        } else if (field == 7 || field == 9 || field == 11 || field == 12 || field == 19 || field == 20) {
            if (parse_u64(token, &value) != 0)
                return -1;
            if (field == 7)
                stats->minor_faults = value;
            else if (field == 9)
                stats->major_faults = value;
            else if (field == 11)
                stats->user_ticks = value;
            else if (field == 12)
                stats->system_ticks = value;
            else if (field == 19)
                stats->start_time_ticks = value;
            else
                stats->virtual_bytes = value;
        } else if (field == 21) {
            stats->resident_pages = strtoll(token, NULL, 10);
        } else if (field == 36) {
            stats->cpu = (int)strtol(token, NULL, 10);
        }
        field++;
        token = strtok_r(NULL, " ", &save);
    }
    return field > 36 ? 0 : -1;
}

static int read_proc_stat_path(const char *path, process_stats_t *stats)
{
    FILE *file;
    char line[PROC_LINE_SIZE];

    file = fopen(path, "r");
    if (file == NULL)
        return -1;
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);
    memset(stats, 0, sizeof(*stats));
    stats->cpu = -1;
    return parse_proc_stat_line(line, stats);
}

static int read_process_stat(pid_t pid, process_stats_t *stats)
{
    monitor_profile_scope_t profile;
    char path[64];
    int result;

    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    monitor_profile_scope_begin(&profile, "sample", "proc_pid_stat", NULL, pid, 0);
    result = read_proc_stat_path(path, stats);
    monitor_profile_scope_end(&profile, result == 0 ? "OK" : "ERROR");
    return result;
}

static int read_thread_stat(pid_t pid, pid_t tid, process_stats_t *stats)
{
    monitor_profile_scope_t profile;
    char path[96];
    int result;

    snprintf(path, sizeof(path), "/proc/%ld/task/%ld/stat", (long)pid, (long)tid);
    monitor_profile_scope_begin(&profile, "sample", "per_thread_stat", NULL, pid, (uint64_t)tid);
    result = read_proc_stat_path(path, stats);
    monitor_profile_scope_end(&profile, result == 0 ? "OK" : "ERROR");
    return result;
}

static int read_memory_status(pid_t pid, int64_t *rss_kb, int64_t *vms_kb)
{
    monitor_profile_scope_t profile;
    char path[64];
    char line[256];
    FILE *file;
    int result;

    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    monitor_profile_scope_begin(&profile, "sample", "memory_rss", NULL, pid, 0);
    file = fopen(path, "r");
    if (file == NULL) {
        monitor_profile_scope_end(&profile, "ERROR");
        return -1;
    }
    *rss_kb = -1;
    *vms_kb = -1;
    while (fgets(line, sizeof(line), file) != NULL) {
        long long value;

        if (sscanf(line, "VmRSS: %lld kB", &value) == 1)
            *rss_kb = value;
        else if (sscanf(line, "VmSize: %lld kB", &value) == 1)
            *vms_kb = value;
    }
    fclose(file);
    result = *rss_kb >= 0 && *vms_kb >= 0 ? 0 : -1;
    monitor_profile_scope_end(&profile, result == 0 ? "OK" : "ERROR");
    return result;
}

static bool numeric_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;

    if (*cursor == '\0')
        return false;
    while (*cursor != '\0') {
        if (*cursor < '0' || *cursor > '9')
            return false;
        cursor++;
    }
    return true;
}

static int history_index(const monitor_state_t *state, pid_t tid)
{
    size_t index;

    for (index = 0; index < state->history_count; index++)
        if (state->history[index].tid == tid)
            return (int)index;
    return -1;
}

static int read_threads(monitor_state_t *state, monitor_sample_t *sample, uint64_t system_delta)
{
    monitor_profile_scope_t total_profile;
    char path[64];
    DIR *directory;
    struct dirent *entry;
    thread_sample_t *samples = NULL;
    thread_history_t *new_history = NULL;
    size_t count = 0;

    snprintf(path, sizeof(path), "/proc/%ld/task", (long)state->pid);
    monitor_profile_scope_begin(&total_profile, "sample", "thread_statistics_total", NULL, state->pid, 0);
    directory = opendir(path);
    if (directory == NULL) {
        monitor_profile_scope_end(&total_profile, "ERROR");
        return -1;
    }
    while (1) {
        process_stats_t thread_stats;
        thread_sample_t *sample_slot;
        thread_history_t *history_slot;
        pid_t tid;
        int previous;
        monitor_profile_scope_t enumeration_profile;

        monitor_profile_scope_begin(&enumeration_profile, "sample", "thread_enumeration", NULL, state->pid, 0);
        entry = readdir(directory);
        monitor_profile_scope_end(&enumeration_profile, entry == NULL ? "END" : "OK");
        if (entry == NULL)
            break;
        if (!numeric_name(entry->d_name))
            continue;
        tid = (pid_t)strtol(entry->d_name, NULL, 10);
        if (read_thread_stat(state->pid, tid, &thread_stats) != 0)
            continue;
        sample_slot = realloc(samples, (count + 1) * sizeof(*samples));
        if (sample_slot == NULL) {
            free(samples);
            free(new_history);
            closedir(directory);
            monitor_profile_scope_end(&total_profile, "ERROR");
            return -1;
        }
        samples = sample_slot;
        history_slot = realloc(new_history, (count + 1) * sizeof(*new_history));
        if (history_slot == NULL) {
            free(samples);
            free(new_history);
            closedir(directory);
            monitor_profile_scope_end(&total_profile, "ERROR");
            return -1;
        }
        new_history = history_slot;
        previous = history_index(state, tid);
        samples[count].tid = tid;
        samples[count].cpu = thread_stats.cpu;
        samples[count].cpu_node = state->numa_available && thread_stats.cpu >= 0
                                       ? numa_node_of_cpu(thread_stats.cpu)
                                       : -1;
        samples[count].state = thread_stats.state;
        if (previous >= 0 && system_delta > 0) {
            uint64_t old_ticks = state->history[previous].user_ticks +
                                 state->history[previous].system_ticks;
            uint64_t new_ticks = thread_stats.user_ticks + thread_stats.system_ticks;
            samples[count].cpu_utilization = (double)(new_ticks - old_ticks) /
                                              (double)system_delta * state->cpu_count * 100.0;
        } else {
            samples[count].cpu_utilization = UNAVAILABLE_INT64;
        }
        new_history[count].tid = tid;
        new_history[count].user_ticks = thread_stats.user_ticks;
        new_history[count].system_ticks = thread_stats.system_ticks;
        count++;
    }
    closedir(directory);
    free(state->history);
    state->history = new_history;
    state->history_count = count;
    sample->threads = samples;
    sample->thread_count = count;
    monitor_profile_scope_end(&total_profile, "OK");
    return 0;
}

static int read_numa_summary(pid_t pid, char *summary, size_t summary_size)
{
    monitor_profile_scope_t profile;
    uint64_t node_pages[128] = {0};
    char path[64];
    char line[1024];
    FILE *file;
    bool found = false;
    size_t used = 0;
    int node;

    monitor_profile_scope_begin(&profile, "sample", "numa_information", NULL, pid, 0);
    snprintf(path, sizeof(path), "/proc/%ld/numa_maps", (long)pid);
    file = fopen(path, "r");
    if (file == NULL) {
        snprintf(summary, summary_size, "NA");
        monitor_profile_scope_end(&profile, "UNAVAILABLE");
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *token;
        char *save = NULL;

        token = strtok_r(line, " \t\n", &save);
        while (token != NULL) {
            unsigned parsed_node;
            unsigned long long pages;
            char *equals = strchr(token, '=');

            if (equals != NULL && token[0] == 'N' &&
                sscanf(token, "N%u=%llu", &parsed_node, &pages) == 2 && parsed_node < 128) {
                node_pages[parsed_node] += pages;
                found = true;
            }
            token = strtok_r(NULL, " \t\n", &save);
        }
    }
    fclose(file);
    if (!found) {
        snprintf(summary, summary_size, "NA");
        monitor_profile_scope_end(&profile, "UNAVAILABLE");
        return -1;
    }
    summary[0] = '\0';
    for (node = 0; node < 128; node++) {
        int written;

        if (node_pages[node] == 0)
            continue;
        written = snprintf(summary + used, summary_size - used, "%sN%d=%" PRIu64,
                           used == 0 ? "" : ";", node, node_pages[node]);
        if (written < 0 || (size_t)written >= summary_size - used) {
            break;
        }
        used += (size_t)written;
    }
    monitor_profile_scope_end(&profile, "OK");
    return 0;
}

static long long read_counter(int fd, bool available)
{
    monitor_profile_scope_t profile;
    uint64_t value;
    long long result;

    monitor_profile_scope_begin(&profile, "sample", "cache_perf_read", NULL, -1, 0);
    if (!available || read(fd, &value, sizeof(value)) != (ssize_t)sizeof(value))
        result = UNAVAILABLE_INT64;
    else
        result = (long long)value;
    monitor_profile_scope_end(&profile, result == UNAVAILABLE_INT64 ? "UNAVAILABLE" : "OK");
    return result;
}

static int open_perf_counter(pid_t pid, uint64_t event, char *reason, size_t reason_size)
{
    monitor_profile_scope_t profile;
    struct perf_event_attr attributes;
    int fd;

    monitor_profile_scope_begin(&profile, "lifecycle", "perf_counter_attempt", NULL, pid, 0);

    memset(&attributes, 0, sizeof(attributes));
    attributes.type = PERF_TYPE_HARDWARE;
    attributes.size = sizeof(attributes);
    attributes.config = event;
    attributes.disabled = 1;
    attributes.inherit = 1;
    fd = (int)syscall(__NR_perf_event_open, &attributes, pid, -1, -1, 0);
    if (fd < 0) {
        snprintf(reason, reason_size, "%s", strerror(errno));
        monitor_profile_scope_end(&profile, "UNAVAILABLE");
        return -1;
    }
    if (ioctl(fd, PERF_EVENT_IOC_RESET, 0) != 0 || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        snprintf(reason, reason_size, "%s", strerror(errno));
        close(fd);
        monitor_profile_scope_end(&profile, "UNAVAILABLE");
        return -1;
    }
    monitor_profile_scope_end(&profile, "OK");
    return fd;
}

static void initialize_perf_counters(monitor_state_t *state)
{
    monitor_profile_scope_t profile;

    monitor_profile_scope_begin(&profile, "lifecycle", "perf_counter_initialization", NULL, state->pid, 0);
    state->counters.references_fd = open_perf_counter(state->pid, PERF_COUNT_HW_CACHE_REFERENCES,
                                                       state->counters.references_reason,
                                                       sizeof(state->counters.references_reason));
    state->counters.misses_fd = open_perf_counter(state->pid, PERF_COUNT_HW_CACHE_MISSES,
                                                  state->counters.misses_reason,
                                                  sizeof(state->counters.misses_reason));
    state->counters.references_available = state->counters.references_fd >= 0;
    state->counters.misses_available = state->counters.misses_fd >= 0;
    monitor_profile_scope_end(&profile, "OK");
}

static void close_perf_counters(monitor_state_t *state)
{
    if (state->counters.references_fd >= 0)
        close(state->counters.references_fd);
    if (state->counters.misses_fd >= 0)
        close(state->counters.misses_fd);
}

static int open_csv(const char *path, const char *header, FILE **result)
{
    FILE *file;
    char existing[2048];

    file = fopen(path, "a+");
    if (file == NULL)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    if (ftell(file) == 0) {
        fprintf(file, "%s\n", header);
    } else {
        rewind(file);
        if (fgets(existing, sizeof(existing), file) == NULL) {
            fclose(file);
            return -1;
        }
        existing[strcspn(existing, "\r\n")] = '\0';
        if (strcmp(existing, header) != 0) {
            fclose(file);
            errno = EINVAL;
            return -1;
        }
        fseek(file, 0, SEEK_END);
    }
    *result = file;
    return 0;
}

static void human_timestamp(char *buffer, size_t size)
{
    struct timespec now;
    struct tm utc;

    clock_gettime(CLOCK_REALTIME, &now);
    gmtime_r(&now.tv_sec, &utc);
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &utc);
}

static int collect_sample(monitor_state_t *state, monitor_sample_t *sample)
{
    uint64_t system_delta = 0;
    uint64_t process_previous_ticks;
    uint64_t process_current_ticks;
    uint64_t total_delta;
    uint64_t busy_delta;

    memset(sample, 0, sizeof(*sample));
    sample->system_cpu_utilization = UNAVAILABLE_INT64;
    sample->process_cpu_utilization = UNAVAILABLE_INT64;
    sample->rss_kb = -1;
    sample->vms_kb = -1;
    sample->rss_delta_kb = -1;
    sample->interval_minor_faults = -1;
    sample->interval_major_faults = -1;
    sample->cache_references = read_counter(state->counters.references_fd,
                                            state->counters.references_available);
    sample->cache_misses = read_counter(state->counters.misses_fd,
                                        state->counters.misses_available);
    if (read_process_stat(state->pid, &sample->process) != 0 ||
        read_system_cpu(&sample->system) != 0 ||
        read_memory_status(state->pid, &sample->rss_kb, &sample->vms_kb) != 0)
        return -1;
    read_numa_summary(state->pid, sample->numa_summary, sizeof(sample->numa_summary));
    if (state->have_previous_system) {
        uint64_t current_total = total_cpu_ticks(&sample->system);
        uint64_t previous_total = total_cpu_ticks(&state->previous_system);

        total_delta = current_total >= previous_total ? current_total - previous_total : 0;
        busy_delta = busy_cpu_ticks(&sample->system);
        busy_delta = busy_delta >= busy_cpu_ticks(&state->previous_system)
                         ? busy_delta - busy_cpu_ticks(&state->previous_system) : 0;
        if (total_delta > 0)
            sample->system_cpu_utilization = (double)busy_delta / (double)total_delta * 100.0;
        system_delta = total_delta;
    }
    if (state->have_previous_process) {
        process_previous_ticks = state->previous_process.user_ticks + state->previous_process.system_ticks;
        process_current_ticks = sample->process.user_ticks + sample->process.system_ticks;
        sample->interval_minor_faults = sample->process.minor_faults >= state->previous_process.minor_faults
                                            ? (int64_t)(sample->process.minor_faults - state->previous_process.minor_faults) : -1;
        sample->interval_major_faults = sample->process.major_faults >= state->previous_process.major_faults
                                            ? (int64_t)(sample->process.major_faults - state->previous_process.major_faults) : -1;
        if (system_delta > 0)
            sample->process_cpu_utilization = (double)(process_current_ticks - process_previous_ticks) /
                                              (double)system_delta * state->cpu_count * 100.0;
        sample->rss_delta_kb = sample->rss_kb - state->previous_rss_kb;
    }
    if (read_threads(state, sample, system_delta) != 0)
        return -1;
    sample->elapsed_ms = (uint64_t)((monotonic_seconds() - state->start_monotonic) * 1000.0);
    state->previous_process = sample->process;
    state->previous_system = sample->system;
    state->previous_rss_kb = sample->rss_kb;
    state->have_previous_process = true;
    state->have_previous_system = true;
    return 0;
}

static void write_sample(monitor_state_t *state, const monitor_sample_t *sample,
                         const char *timestamp)
{
    monitor_profile_scope_t profile;
    const monitor_metadata_t *metadata = &state->config->metadata;

    monitor_profile_scope_begin(&profile, "sample", "csv_formatting_process", NULL, state->pid, 0);
    fprintf(state->output,
            "%s,%" PRIu64 ",%ld,%.3f,%.3f,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRId64 ",%lld,%lld,%zu,%d,%s,%s,%s,%s,%s,%s\n",
            timestamp, sample->elapsed_ms, (long)state->pid,
            sample->system_cpu_utilization, sample->process_cpu_utilization,
            sample->rss_kb, sample->vms_kb, sample->rss_delta_kb,
            sample->process.minor_faults, sample->process.major_faults,
            sample->interval_minor_faults, sample->interval_major_faults,
            sample->cache_references, sample->cache_misses, sample->thread_count,
            state->numa_nodes, sample->numa_summary,
            metadata->pattern != NULL ? metadata->pattern : "NA",
            metadata->threads != NULL ? metadata->threads : "NA",
            metadata->memory_mb != NULL ? metadata->memory_mb : "NA",
            metadata->iterations != NULL ? metadata->iterations : "NA",
            metadata->duration != NULL ? metadata->duration : "NA");
    monitor_profile_scope_end(&profile, "OK");
    monitor_profile_scope_begin(&profile, "sample", "csv_write_process", NULL, state->pid, 0);
    fflush(state->output);
    monitor_profile_scope_end(&profile, "OK");
    monitor_profile_scope_begin(&profile, "sample", "csv_formatting_threads", NULL, state->pid, 0);
    for (size_t index = 0; index < sample->thread_count; index++) {
        const thread_sample_t *thread = &sample->threads[index];

        fprintf(state->thread_output, "%s,%" PRIu64 ",%ld,%ld,%d,%d,%.3f,%c\n",
                timestamp, sample->elapsed_ms, (long)state->pid, (long)thread->tid,
                thread->cpu, thread->cpu_node, thread->cpu_utilization, thread->state);
    }
    monitor_profile_scope_end(&profile, "OK");
    monitor_profile_scope_begin(&profile, "sample", "csv_write_threads", NULL, state->pid, 0);
    fflush(state->thread_output);
    monitor_profile_scope_end(&profile, "OK");
}

static void print_availability(const monitor_state_t *state)
{
    printf("Metric Availability\n");
    printf("-------------------\n");
    printf("CPU utilization       : AVAILABLE\n");
    printf("Memory statistics     : AVAILABLE\n");
    printf("Page faults           : AVAILABLE\n");
    printf("Cache references      : %s", state->counters.references_available ? "AVAILABLE" : "UNAVAILABLE");
    if (!state->counters.references_available)
        printf(" (%s)", state->counters.references_reason);
    printf("\n");
    printf("Cache misses          : %s", state->counters.misses_available ? "AVAILABLE" : "UNAVAILABLE");
    if (!state->counters.misses_available)
        printf(" (%s)", state->counters.misses_reason);
    printf("\n");
    printf("NUMA information      : %s\n", state->numa_available ? "AVAILABLE" : "UNAVAILABLE");
}

static bool target_alive(monitor_state_t *state)
{
    char path[64];

    if (state->config->target_is_child) {
        int status;
        pid_t waited = waitpid(state->pid, &status, WNOHANG);

        if (waited == state->pid) {
            if (state->config->child_status != NULL)
                *state->config->child_status = status;
            return false;
        }
        if (waited < 0 && errno != EINTR)
            return false;
        return true;
    }
    snprintf(path, sizeof(path), "/proc/%ld", (long)state->pid);
    if (access(path, F_OK) != 0)
        return false;
    {
        process_stats_t process;

        if (read_process_stat(state->pid, &process) != 0)
            return false;
        return process.state != 'Z' && process.state != 'X' &&
               (!state->config->expected_start_time_available ||
                process.start_time_ticks == state->config->expected_start_time_ticks);
    }
}

static void sleep_interval(unsigned interval_ms, volatile sig_atomic_t *stop_requested)
{
    monitor_profile_scope_t profile;
    struct timespec requested;

    monitor_profile_scope_begin(&profile, "session", "sampling_interval_sleep", NULL, -1, 0);
    requested.tv_sec = interval_ms / 1000U;
    requested.tv_nsec = (long)(interval_ms % 1000U) * 1000000L;
    while (nanosleep(&requested, &requested) != 0 && errno == EINTR) {
        if (stop_requested != NULL && *stop_requested)
            break;
    }
    monitor_profile_scope_end(&profile, "OK");
}

static void cleanup_monitor_state(monitor_state_t *state);

static int initialize_monitor_state(const monitor_config_t *config,
                                    monitor_state_t *state)
{
    if (config == NULL || config->pid <= 0 || config->interval_ms == 0 ||
        config->output_path == NULL || config->thread_output_path == NULL)
        return -1;
    if (!target_alive(&(monitor_state_t){.pid = config->pid, .config = config}))
        return MONITOR_RESULT_TARGET_GONE;
    memset(state, 0, sizeof(*state));
    state->pid = config->pid;
    state->interval_ms = config->interval_ms;
    state->ticks_per_second = sysconf(_SC_CLK_TCK);
    state->cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    state->cpu_count = state->cpu_count > 0 ? state->cpu_count : 1;
    state->config = config;
    state->start_monotonic = monotonic_seconds();
    state->counters.references_fd = -1;
    state->counters.misses_fd = -1;
    state->numa_available = numa_available() >= 0;
    state->numa_nodes = state->numa_available ? numa_num_configured_nodes() : -1;
    if (config->log_path != NULL) {
        state->log = fopen(config->log_path, "a");
        if (state->log == NULL)
            fprintf(stderr, "Warning: cannot open monitor log '%s': %s\n",
                    config->log_path, strerror(errno));
    }
    if (open_csv(config->output_path, monitor_csv_header(), &state->output) != 0 ||
        open_csv(config->thread_output_path, thread_csv_header(), &state->thread_output) != 0) {
        fprintf(stderr, "Error: cannot open monitoring CSV output: %s\n", strerror(errno));
        cleanup_monitor_state(state);
        return MONITOR_RESULT_ERROR;
    }
    initialize_perf_counters(state);
    return 0;
}

static void cleanup_monitor_state(monitor_state_t *state)
{
    free(state->history);
    close_perf_counters(state);
    if (state->output != NULL)
        fclose(state->output);
    if (state->thread_output != NULL)
        fclose(state->thread_output);
    if (state->log != NULL)
        fclose(state->log);
}

int monitor_run_pid_once(const monitor_config_t *config)
{
    monitor_profile_scope_t total_profile;
    monitor_profile_scope_t lifecycle_profile;
    monitor_profile_scope_t sample_profile;
    monitor_state_t state;
    monitor_sample_t sample;
    char timestamp[32];
    int init_result;

    monitor_profile_scope_begin(&total_profile, "lifecycle", "monitor_run_pid_once", NULL,
                                config == NULL ? -1 : config->pid, 0);
    monitor_profile_scope_begin(&lifecycle_profile, "lifecycle", "initialization", NULL,
                                config == NULL ? -1 : config->pid, 0);
    init_result = initialize_monitor_state(config, &state);
    monitor_profile_scope_end(&lifecycle_profile, init_result == 0 ? "OK" : "ERROR");
    if (init_result != 0) {
        if (config != NULL && config->pid > 0)
            fprintf(stderr, "Error: target PID %ld is unavailable for one sample.\n", (long)config->pid);
        monitor_profile_scope_end(&total_profile, "ERROR");
        return init_result == MONITOR_RESULT_TARGET_GONE ?
                   MONITOR_RESULT_TARGET_GONE : MONITOR_RESULT_ERROR;
    }
    monitor_profile_scope_begin(&sample_profile, "sample", "monitor_sample_total", NULL, state.pid, 0);
    if (collect_sample(&state, &sample) != 0) {
        int result = target_alive(&state) ? MONITOR_RESULT_ERROR : MONITOR_RESULT_TARGET_GONE;

        free(sample.threads);
        monitor_profile_scope_end(&sample_profile, "ERROR");
        monitor_profile_scope_begin(&lifecycle_profile, "lifecycle", "finalization", NULL, state.pid, 0);
        cleanup_monitor_state(&state);
        monitor_profile_scope_end(&lifecycle_profile, "OK");
        monitor_profile_scope_end(&total_profile, "ERROR");
        return result;
    }
    if (!target_alive(&state)) {
        free(sample.threads);
        monitor_profile_scope_end(&sample_profile, "TARGET_GONE");
        monitor_profile_scope_begin(&lifecycle_profile, "lifecycle", "finalization", NULL, state.pid, 0);
        cleanup_monitor_state(&state);
        monitor_profile_scope_end(&lifecycle_profile, "OK");
        monitor_profile_scope_end(&total_profile, "TARGET_GONE");
        return MONITOR_RESULT_TARGET_GONE;
    }
    human_timestamp(timestamp, sizeof(timestamp));
    write_sample(&state, &sample, timestamp);
    monitor_profile_scope_end(&sample_profile, "OK");
    free(sample.threads);
    monitor_profile_scope_begin(&lifecycle_profile, "lifecycle", "finalization", NULL, state.pid, 0);
    cleanup_monitor_state(&state);
    monitor_profile_scope_end(&lifecycle_profile, "OK");
    monitor_profile_scope_end(&total_profile, "OK");
    return 0;
}

int monitor_run_pid(const monitor_config_t *config)
{
    monitor_profile_scope_t session_profile;
    monitor_profile_scope_t lifecycle_profile;
    monitor_profile_scope_t sample_profile;
    monitor_state_t state;
    monitor_sample_t sample;
    char timestamp[32];
    unsigned sample_attempt;
    bool sample_collected;
    int result = -1;
    int init_result;

    monitor_profile_scope_begin(&session_profile, "lifecycle", "monitor_session", NULL,
                                config == NULL ? -1 : config->pid, 0);
    monitor_profile_scope_begin(&lifecycle_profile, "lifecycle", "initialization", NULL,
                                config == NULL ? -1 : config->pid, 0);
    init_result = initialize_monitor_state(config, &state);
    monitor_profile_scope_end(&lifecycle_profile, init_result == 0 ? "OK" : "ERROR");
    if (init_result != 0) {
        if (config != NULL && config->pid > 0) {
            fprintf(stderr, "Error: target PID %ld does not exist.\n", (long)config->pid);
        }
        monitor_profile_scope_end(&session_profile, "ERROR");
        return -1;
    }
    print_availability(&state);
    if (state.log != NULL) {
        fprintf(state.log, "target_pid=%ld interval_ms=%u\n", (long)state.pid, state.interval_ms);
        fprintf(state.log, "numa_nodes=%d sampling_started=true\n", state.numa_nodes);
        fprintf(state.log, "cache_references=%s (%s)\n", state.counters.references_available ? "available" : "unavailable", state.counters.references_reason);
        fprintf(state.log, "cache_misses=%s (%s)\n", state.counters.misses_available ? "available" : "unavailable", state.counters.misses_reason);
        fflush(state.log);
    }
    printf("Monitoring started for PID %ld at %u ms intervals.\n", (long)state.pid, state.interval_ms);
    while (!(config->stop_requested != NULL && *config->stop_requested)) {
        if (!target_alive(&state))
            break;
        monitor_profile_scope_begin(&sample_profile, "sample", "monitor_sample_total", NULL, state.pid, 0);
        sample_collected = false;
        for (sample_attempt = 0; sample_attempt < 3; sample_attempt++) {
            memset(&sample, 0, sizeof(sample));
            if (collect_sample(&state, &sample) == 0) {
                sample_collected = true;
                break;
            }
            free(sample.threads);
            sample.threads = NULL;
            if (!target_alive(&state))
                break;
            if (sample_attempt + 1 < 3)
                sleep_interval(1, config->stop_requested);
        }
        if (!sample_collected) {
            monitor_profile_scope_end(&sample_profile, "ERROR");
            if (!target_alive(&state))
                break;
            fprintf(stderr, "Error: unable to collect a complete monitoring sample.\n");
            goto cleanup;
        }
        human_timestamp(timestamp, sizeof(timestamp));
        write_sample(&state, &sample, timestamp);
        monitor_profile_scope_end(&sample_profile, "OK");
        printf("Sample: elapsed=%" PRIu64 " ms, process CPU=%.2f%%, system CPU=%.2f%%, RSS=%" PRId64 " KB, minor faults=%" PRIu64 ", major faults=%" PRIu64 ", threads=%zu\n",
               sample.elapsed_ms, sample.process_cpu_utilization, sample.system_cpu_utilization,
               sample.rss_kb, sample.process.minor_faults, sample.process.major_faults,
               sample.thread_count);
        free(sample.threads);
        state.sample_count++;
        sleep_interval(state.interval_ms, config->stop_requested);
    }
    printf("Monitoring completed after %" PRIu64 " samples.\n", state.sample_count);
    if (state.log != NULL && config->target_is_child && config->child_status != NULL &&
        *config->child_status >= 0)
        fprintf(state.log, "benchmark_exit_status=%d\n", *config->child_status);
    log_line(&state, "monitoring stopped");
    result = 0;

cleanup:
    monitor_profile_scope_begin(&lifecycle_profile, "lifecycle", "finalization", NULL, state.pid, 0);
    cleanup_monitor_state(&state);
    monitor_profile_scope_end(&lifecycle_profile, "OK");
    monitor_profile_scope_end(&session_profile, result == 0 ? "OK" : "ERROR");
    return result;
}
