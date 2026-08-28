#define _POSIX_C_SOURCE 200809L

#include "monitor_profile.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_once_t profile_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t profile_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *profile_file;

uint64_t monitor_profile_now_ns(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static void profile_initialize(void)
{
    const char *path = getenv("AWAVMA_PROFILE_PATH");

    if (path == NULL || path[0] == '\0')
        return;
    profile_file = fopen(path, "a");
    if (profile_file == NULL)
        return;
    if (ftell(profile_file) == 0) {
        fputs("level,component,application,pid,job_id,start_ns,end_ns,duration_us,status,metric_value\n",
              profile_file);
    }
}

void monitor_profile_event(const char *level, const char *component,
                           const char *application, pid_t pid,
                           uint64_t job_id, uint64_t start_ns,
                           uint64_t end_ns, const char *status)
{
    pthread_once(&profile_once, profile_initialize);
    if (profile_file == NULL)
        return;
    pthread_mutex_lock(&profile_mutex);
    fprintf(profile_file, "%s,%s,%s,%ld,%llu,%llu,%llu,%.3f,%s,\n",
            level == NULL ? "unknown" : level,
            component == NULL ? "unknown" : component,
            application == NULL ? "-" : application,
            (long)pid, (unsigned long long)job_id,
            (unsigned long long)start_ns, (unsigned long long)end_ns,
            end_ns >= start_ns ? (double)(end_ns - start_ns) / 1000.0 : 0.0,
            status == NULL ? "OK" : status);
    pthread_mutex_unlock(&profile_mutex);
}

void monitor_profile_counter(const char *level, const char *component,
                             const char *application, pid_t pid,
                             uint64_t job_id, uint64_t value,
                             const char *status)
{
    uint64_t now = monitor_profile_now_ns();

    pthread_once(&profile_once, profile_initialize);
    if (profile_file == NULL)
        return;
    pthread_mutex_lock(&profile_mutex);
    fprintf(profile_file, "%s,%s,%s,%ld,%llu,%llu,%llu,0.000,%s,%llu\n",
            level == NULL ? "unknown" : level,
            component == NULL ? "unknown" : component,
            application == NULL ? "-" : application,
            (long)pid, (unsigned long long)job_id,
            (unsigned long long)now, (unsigned long long)now,
            status == NULL ? "OK" : status, (unsigned long long)value);
    pthread_mutex_unlock(&profile_mutex);
}

void monitor_profile_scope_begin(monitor_profile_scope_t *scope,
                                 const char *level, const char *component,
                                 const char *application, pid_t pid,
                                 uint64_t job_id)
{
    if (scope == NULL)
        return;
    scope->level = level;
    scope->component = component;
    scope->application = application;
    scope->pid = pid;
    scope->job_id = job_id;
    scope->start_ns = monitor_profile_now_ns();
}

void monitor_profile_scope_end(monitor_profile_scope_t *scope,
                               const char *status)
{
    uint64_t end_ns;

    if (scope == NULL)
        return;
    end_ns = monitor_profile_now_ns();
    monitor_profile_event(scope->level, scope->component, scope->application,
                          scope->pid, scope->job_id, scope->start_ns, end_ns,
                          status);
}
