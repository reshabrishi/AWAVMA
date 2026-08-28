#define _POSIX_C_SOURCE 200809L

#include "runtime_target_filter.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROC_STAT_BUFFER_SIZE 4096U

static int start_time_for_pid(pid_t pid, uint64_t *start_time_ticks)
{
    char path[64];
    char line[PROC_STAT_BUFFER_SIZE];
    char *closing;
    char *token;
    char *save = NULL;
    FILE *file;
    unsigned field = 3;

    if (pid <= 0 || start_time_ticks == NULL)
        return EINVAL;
    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path))
        return ENAMETOOLONG;
    file = fopen(path, "r");
    if (file == NULL)
        return errno == 0 ? ENOENT : errno;
    if (fgets(line, sizeof(line), file) == NULL) {
        int result = ferror(file) ? EIO : ENOENT;

        fclose(file);
        return result;
    }
    fclose(file);
    closing = strrchr(line, ')');
    if (closing == NULL || closing[1] != ' ')
        return EINVAL;
    token = strtok_r(closing + 2, " \t\r\n", &save);
    while (token != NULL) {
        if (field == 3 && token[0] == 'Z')
            return ESRCH;
        if (field == 22) {
            char *end = NULL;
            unsigned long long value;

            errno = 0;
            value = strtoull(token, &end, 10);
            if (errno != 0 || end == token || *end != '\0')
                return EINVAL;
            *start_time_ticks = (uint64_t)value;
            return 0;
        }
        field++;
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    return EINVAL;
}

void runtime_target_filter_init(runtime_target_filter_t *filter)
{
    if (filter != NULL)
        memset(filter, 0, sizeof(*filter));
}

void runtime_target_filter_cleanup(runtime_target_filter_t *filter)
{
    if (filter == NULL)
        return;
    free(filter->identities);
    memset(filter, 0, sizeof(*filter));
}

int runtime_target_filter_add_identity(runtime_target_filter_t *filter, pid_t pid,
                                       uint64_t start_time_ticks)
{
    runtime_target_identity_t *expanded;

    if (filter == NULL || pid <= 0)
        return EINVAL;
    for (size_t index = 0; index < filter->count; index++)
        if (filter->identities[index].pid == pid)
            return filter->identities[index].start_time_ticks == start_time_ticks ? 0 : EEXIST;
    if (filter->count == filter->capacity) {
        size_t capacity = filter->capacity == 0 ? 4U : filter->capacity * 2U;

        if (capacity < filter->capacity || capacity > SIZE_MAX / sizeof(*expanded))
            return ENOMEM;
        expanded = realloc(filter->identities, capacity * sizeof(*expanded));
        if (expanded == NULL)
            return ENOMEM;
        filter->identities = expanded;
        filter->capacity = capacity;
    }
    filter->identities[filter->count++] = (runtime_target_identity_t){pid, start_time_ticks};
    return 0;
}

int runtime_target_filter_add_pid(runtime_target_filter_t *filter, pid_t pid)
{
    uint64_t start_time_ticks;
    int result;

    if (filter == NULL || pid <= 0)
        return EINVAL;
    for (size_t index = 0; index < filter->count; index++)
        if (filter->identities[index].pid == pid)
            return 0;
    result = start_time_for_pid(pid, &start_time_ticks);
    if (result != 0)
        return result;
    return runtime_target_filter_add_identity(filter, pid, start_time_ticks);
}

bool runtime_target_filter_matches(const application_manager_record_t *application,
                                   void *context)
{
    const runtime_target_filter_t *filter = context;

    if (application == NULL || filter == NULL)
        return false;
    for (size_t index = 0; index < filter->count; index++)
        if (filter->identities[index].pid == application->pid &&
            filter->identities[index].start_time_ticks == application->start_time_ticks)
            return true;
    return false;
}
