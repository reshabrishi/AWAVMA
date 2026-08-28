#define _POSIX_C_SOURCE 200809L

#include "application_discovery.h"
#include "monitor_profile.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PROC_ROOT "/proc"
#define PROC_STAT_BUFFER_SIZE 4096U

struct application_discovery {
    application_discovery_config_t config;
    application_discovery_record_t *records;
    size_t count;
    size_t capacity;
    bool initialized;
};

static bool numeric_name(const char *name)
{
    if (name == NULL || *name == '\0')
        return false;
    while (*name != '\0') {
        if (*name < '0' || *name > '9')
            return false;
        name++;
    }
    return true;
}

static int parse_pid(const char *text, pid_t *pid)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > (unsigned long)INT_MAX)
        return -1;
    *pid = (pid_t)value;
    return 0;
}

static int parse_unsigned_long_long(const char *text, uint64_t *value)
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

static int parse_parent_pid(const char *text, pid_t *pid)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > INT_MAX)
        return -1;
    *pid = (pid_t)value;
    return 0;
}

static int path_for_pid(char *buffer, size_t size, const char *root, pid_t pid,
                        const char *file)
{
    int written = snprintf(buffer, size, "%s/%ld/%s", root, (long)pid, file);

    return written < 0 || (size_t)written >= size ? -1 : 0;
}

static int read_process_stat(const char *path, pid_t pid,
                             application_discovery_record_t *record)
{
    FILE *file;
    char line[PROC_STAT_BUFFER_SIZE];
    const char *opening;
    const char *closing;
    char fields[PROC_STAT_BUFFER_SIZE];
    char *token;
    char *save = NULL;
    unsigned field = 3;
    bool have_state = false;
    bool have_parent = false;
    bool have_start_time = false;

    file = fopen(path, "r");
    if (file == NULL)
        return -1;
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);

    opening = strchr(line, '(');
    closing = strrchr(line, ')');
    if (opening == NULL || closing == NULL || opening >= closing || closing[1] != ' ')
        return -1;
    if ((size_t)(closing - opening - 1) >= sizeof(record->process_name))
        return -1;
    memcpy(record->process_name, opening + 1, (size_t)(closing - opening - 1));
    record->process_name[closing - opening - 1] = '\0';
    if (record->process_name[0] == '[')
        return -2;

    if (strlen(closing + 2) >= sizeof(fields))
        return -1;
    strcpy(fields, closing + 2);
    token = strtok_r(fields, " \t\r\n", &save);
    while (token != NULL) {
        if (field == 3) {
            if (token[0] == '\0')
                return -1;
            record->state = token[0];
            have_state = true;
        } else if (field == 4) {
            if (parse_parent_pid(token, &record->parent_pid) != 0)
                return -1;
            have_parent = true;
        } else if (field == 22) {
            if (parse_unsigned_long_long(token, &record->start_time_ticks) != 0)
                return -1;
            have_start_time = true;
            break;
        }
        field++;
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    if (!have_state || !have_parent || !have_start_time || record->state == 'Z')
        return -1;
    record->pid = pid;
    return 0;
}

static void read_executable(const char *root, application_discovery_record_t *record)
{
    char path[PATH_MAX + 64];
    ssize_t length;

    record->executable_path[0] = '\0';
    if (path_for_pid(path, sizeof(path), root, record->pid, "exe") != 0)
        return;
    length = readlink(path, record->executable_path, sizeof(record->executable_path) - 1);
    if (length < 0) {
        record->executable_path[0] = '\0';
        return;
    }
    record->executable_path[length] = '\0';
}

static bool is_kernel_thread(const char *root, pid_t pid)
{
    char path[PATH_MAX + 64];
    char line[256];
    FILE *file;
    int value;

    if (path_for_pid(path, sizeof(path), root, pid, "status") != 0)
        return false;
    file = fopen(path, "r");
    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "Kthread: %d", &value) == 1) {
            fclose(file);
            return value != 0;
        }
    }
    fclose(file);
    return false;
}

static bool contains_pid(const application_discovery_t *discovery, pid_t pid)
{
    for (size_t index = 0; index < discovery->count; index++)
        if (discovery->records[index].pid == pid)
            return true;
    return false;
}

static int append_record(application_discovery_t *discovery,
                         const application_discovery_record_t *record)
{
    application_discovery_record_t *expanded;

    if (discovery->count == discovery->capacity) {
        size_t capacity = discovery->capacity == 0 ? 32U : discovery->capacity * 2U;

        expanded = realloc(discovery->records, capacity * sizeof(*expanded));
        if (expanded == NULL)
            return ENOMEM;
        discovery->records = expanded;
        discovery->capacity = capacity;
    }
    discovery->records[discovery->count++] = *record;
    return 0;
}

void application_discovery_config_default(application_discovery_config_t *config)
{
    if (config == NULL)
        return;
    config->proc_root = DEFAULT_PROC_ROOT;
    config->include_pid1 = false;
    config->excluded_pid = getpid();
}

application_discovery_t *application_discovery_create(void)
{
    return calloc(1, sizeof(application_discovery_t));
}

int application_discovery_init(application_discovery_t *discovery,
                               const application_discovery_config_t *config)
{
    application_discovery_config_t defaults;

    if (discovery == NULL)
        return EINVAL;
    if (config == NULL) {
        application_discovery_config_default(&defaults);
        config = &defaults;
    }
    if (config->proc_root == NULL || config->proc_root[0] == '\0')
        return EINVAL;
    application_discovery_cleanup(discovery);
    discovery->config = *config;
    discovery->initialized = true;
    return 0;
}

int application_discovery_scan(application_discovery_t *discovery)
{
    monitor_profile_scope_t profile;
    DIR *directory;
    struct dirent *entry;

    if (discovery == NULL || !discovery->initialized)
        return EINVAL;
    monitor_profile_scope_begin(&profile, "discovery", "application_discovery_scan", NULL, -1, 0);
    discovery->count = 0;
    directory = opendir(discovery->config.proc_root);
    if (directory == NULL) {
        monitor_profile_scope_end(&profile, "ERROR");
        return errno;
    }
    while ((entry = readdir(directory)) != NULL) {
        application_discovery_record_t record;
        char path[PATH_MAX + 64];
        pid_t pid;
        int result;

        if (!numeric_name(entry->d_name) || parse_pid(entry->d_name, &pid) != 0)
            continue;
        if ((!discovery->config.include_pid1 && pid == 1) ||
            (discovery->config.excluded_pid > 0 && pid == discovery->config.excluded_pid) ||
            contains_pid(discovery, pid))
            continue;
        if (path_for_pid(path, sizeof(path), discovery->config.proc_root, pid, "stat") != 0)
            continue;
        memset(&record, 0, sizeof(record));
        result = read_process_stat(path, pid, &record);
        if (result != 0)
            continue;
        if (is_kernel_thread(discovery->config.proc_root, pid))
            continue;
        read_executable(discovery->config.proc_root, &record);
        if (append_record(discovery, &record) != 0) {
            closedir(directory);
            monitor_profile_scope_end(&profile, "ERROR");
            return ENOMEM;
        }
    }
    closedir(directory);
    monitor_profile_scope_end(&profile, "OK");
    return 0;
}

size_t application_discovery_count(const application_discovery_t *discovery)
{
    return discovery == NULL || !discovery->initialized ? 0 : discovery->count;
}

bool application_discovery_get(const application_discovery_t *discovery,
                               size_t index, application_discovery_record_t *record)
{
    if (discovery == NULL || record == NULL || !discovery->initialized || index >= discovery->count)
        return false;
    *record = discovery->records[index];
    return true;
}

void application_discovery_cleanup(application_discovery_t *discovery)
{
    if (discovery == NULL)
        return;
    free(discovery->records);
    discovery->records = NULL;
    discovery->count = 0;
    discovery->capacity = 0;
    discovery->initialized = false;
}

void application_discovery_destroy(application_discovery_t *discovery)
{
    if (discovery == NULL)
        return;
    application_discovery_cleanup(discovery);
    free(discovery);
}
