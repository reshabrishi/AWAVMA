#include "migration.h"
#include "migration_log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/mempolicy.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define MAX_EXECUTION_LOCKS 64U
#define MAX_COOLDOWN_ENTRIES 64U

typedef struct {
    bool in_use;
    pid_t pid;
    pid_t tid;
    char entity_id[128];
    struct timespec acquired_at;
} execution_lock_t;

typedef struct {
    bool in_use;
    pid_t pid;
    pid_t tid;
    char entity_id[128];
    struct timespec completed_at;
} cooldown_entry_t;

static MigrationConfig active_config;
static bool initialized;
static pthread_mutex_t execution_mutex = PTHREAD_MUTEX_INITIALIZER;
static execution_lock_t execution_locks[MAX_EXECUTION_LOCKS];
static cooldown_entry_t cooldown_entries[MAX_COOLDOWN_ENTRIES];

static bool current_start_time_matches(pid_t pid, uint64_t expected)
{
    char path[64];
    char line[4096];
    char fields[4096];
    char *closing;
    char *token;
    char *save = NULL;
    FILE *file;
    unsigned field = 3;

    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path))
        return false;
    file = fopen(path, "r");
    if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL)
            fclose(file);
        return false;
    }
    fclose(file);
    closing = strrchr(line, ')');
    if (closing == NULL || closing[1] != ' ' || strlen(closing + 2) >= sizeof(fields))
        return false;
    strcpy(fields, closing + 2);
    token = strtok_r(fields, " \t\r\n", &save);
    while (token != NULL) {
        if (field == 22) {
            char *end = NULL;
            unsigned long long actual;

            errno = 0;
            actual = strtoull(token, &end, 10);
            return errno == 0 && end != token && *end == '\0' && actual == expected;
        }
        field++;
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    return false;
}

const char *MigrationResultName(MigrationResultCode result)
{
    switch (result) {
    case MIGRATION_SUCCESS: return "MIGRATION_SUCCESS";
    case MIGRATION_NO_ACTION: return "MIGRATION_NO_ACTION";
    case MIGRATION_NO_MIGRATION_REQUIRED: return "MIGRATION_NO_MIGRATION_REQUIRED";
    case MIGRATION_NOT_AUTHORIZED: return "MIGRATION_NOT_AUTHORIZED";
    case MIGRATION_INSUFFICIENT_INFORMATION: return "MIGRATION_INSUFFICIENT_INFORMATION";
    case MIGRATION_TARGET_GONE: return "MIGRATION_TARGET_GONE";
    case MIGRATION_INVALID_SOURCE: return "MIGRATION_INVALID_SOURCE";
    case MIGRATION_INVALID_DESTINATION: return "MIGRATION_INVALID_DESTINATION";
    case MIGRATION_INVALID_TARGET: return "MIGRATION_INVALID_TARGET";
    case MIGRATION_PERMISSION_DENIED: return "MIGRATION_PERMISSION_DENIED";
    case MIGRATION_UNSUPPORTED: return "MIGRATION_UNSUPPORTED";
    case MIGRATION_SYSTEM_ERROR: return "MIGRATION_SYSTEM_ERROR";
    case MIGRATION_PARTIAL_SUCCESS: return "MIGRATION_PARTIAL_SUCCESS";
    case MIGRATION_VERIFICATION_UNAVAILABLE: return "MIGRATION_VERIFICATION_UNAVAILABLE";
    case MIGRATION_ALREADY_IN_PROGRESS: return "MIGRATION_ALREADY_IN_PROGRESS";
    default: return "MIGRATION_SYSTEM_ERROR";
    }
}

const char *MigrationActionName(ValidationAction action)
{
    switch (action) {
    case VALIDATION_ACTION_MOVE_MEMORY: return "MOVE_MEMORY";
    case VALIDATION_ACTION_MOVE_THREAD: return "MOVE_THREAD";
    case VALIDATION_ACTION_NO_MIGRATION: return "NO_MIGRATION";
    default: return "INSUFFICIENT_DECISION_SIGNAL";
    }
}

static bool valid_config(const MigrationConfig *config)
{
    return config != NULL && config->results_path != NULL && config->history_path != NULL &&
           config->log_path != NULL && config->state_path != NULL && config->history_max_records > 0 &&
           isfinite(config->history_max_days) && config->history_max_days > 0.0 &&
           config->cleanup_interval > 0;
}

bool Migration_Init(const MigrationConfig *config)
{
    if (!valid_config(config) || !migration_log_init(config))
        return false;
    active_config = *config;
    initialized = true;
    memset(execution_locks, 0, sizeof(execution_locks));
    memset(cooldown_entries, 0, sizeof(cooldown_entries));
    return true;
}

void Migration_Shutdown(void)
{
    migration_log_shutdown();
    initialized = false;
    memset(&active_config, 0, sizeof(active_config));
    pthread_mutex_lock(&execution_mutex);
    memset(execution_locks, 0, sizeof(execution_locks));
    memset(cooldown_entries, 0, sizeof(cooldown_entries));
    pthread_mutex_unlock(&execution_mutex);
}

static bool same_key(const MigrationRequest *request, pid_t pid, pid_t tid, const char *entity_id)
{
    const char *request_entity = request->phase5_decision.entity_id;

    if (request->pid != pid || request->tid != tid)
        return false;
    if (request->tid != 0)
        return true;
    if (request_entity[0] == '\0' || entity_id == NULL || entity_id[0] == '\0')
        return true;
    return strcmp(request_entity, entity_id) == 0;
}

static long long elapsed_milliseconds(const struct timespec *start, const struct timespec *end)
{
    return (long long)(end->tv_sec - start->tv_sec) * 1000LL +
           (long long)(end->tv_nsec - start->tv_nsec) / 1000000LL;
}

bool Migration_AcquireExecutionLock(const MigrationRequest *request)
{
    size_t free_slot = MAX_EXECUTION_LOCKS;
    struct timespec now;

    if (!initialized || request == NULL)
        return false;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&execution_mutex);
    for (size_t index = 0; index < MAX_EXECUTION_LOCKS; index++) {
        if (execution_locks[index].in_use && same_key(request, execution_locks[index].pid,
                                                       execution_locks[index].tid,
                                                       execution_locks[index].entity_id)) {
            if (active_config.lock_timeout_ms > 0 &&
                elapsed_milliseconds(&execution_locks[index].acquired_at, &now) >=
                (long long)active_config.lock_timeout_ms) {
                memset(&execution_locks[index], 0, sizeof(execution_locks[index]));
                if (free_slot == MAX_EXECUTION_LOCKS)
                    free_slot = index;
                continue;
            }
            pthread_mutex_unlock(&execution_mutex);
            return false;
        }
        if (!execution_locks[index].in_use && free_slot == MAX_EXECUTION_LOCKS)
            free_slot = index;
    }
    if (free_slot == MAX_EXECUTION_LOCKS) {
        pthread_mutex_unlock(&execution_mutex);
        return false;
    }
    execution_locks[free_slot].in_use = true;
    execution_locks[free_slot].pid = request->pid;
    execution_locks[free_slot].tid = request->tid;
    clock_gettime(CLOCK_MONOTONIC, &execution_locks[free_slot].acquired_at);
    snprintf(execution_locks[free_slot].entity_id, sizeof(execution_locks[free_slot].entity_id), "%s",
             request->phase5_decision.entity_id);
    pthread_mutex_unlock(&execution_mutex);
    return true;
}

void Migration_ReleaseExecutionLock(const MigrationRequest *request)
{
    if (!initialized || request == NULL)
        return;
    pthread_mutex_lock(&execution_mutex);
    for (size_t index = 0; index < MAX_EXECUTION_LOCKS; index++) {
        if (execution_locks[index].in_use && same_key(request, execution_locks[index].pid,
                                                       execution_locks[index].tid,
                                                       execution_locks[index].entity_id)) {
            memset(&execution_locks[index], 0, sizeof(execution_locks[index]));
            break;
        }
    }
    pthread_mutex_unlock(&execution_mutex);
}

static double monotonic_seconds(void)
{
    struct timespec value;

    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void timestamp_now(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm utc;

    gmtime_r(&now, &utc);
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static void report_init(const MigrationRequest *request, MigrationReport *report)
{
    memset(report, 0, sizeof(*report));
    timestamp_now(report->timestamp, sizeof(report->timestamp));
    snprintf(report->migration_id, sizeof(report->migration_id), "%s", request->phase5_decision.migration_id);
    snprintf(report->app_id, sizeof(report->app_id), "%s", request->phase5_decision.app_id);
    snprintf(report->entity_id, sizeof(report->entity_id), "%s", request->phase5_decision.entity_id);
    report->pid = request->pid > 0 ? request->pid : (pid_t)request->phase5_decision.pid;
    report->expected_start_time_ticks = request->start_time_ticks;
    report->tid = request->tid;
    report->action = request->phase5_decision.action;
    snprintf(report->phase5_decision, sizeof(report->phase5_decision), "%s",
             MigrationActionName(request->phase5_decision.action));
    snprintf(report->phase6_validation, sizeof(report->phase6_validation), "%s",
             request->phase6_validation.final_decision[0] != '\0' ?
             request->phase6_validation.final_decision : request->phase6_validation.validation_status);
    report->source_numa_node = request->source_numa_node;
    report->destination_numa_node = request->destination_numa_node;
    report->destination_cpu = request->destination_cpu;
    snprintf(report->verification_status, sizeof(report->verification_status), "NOT_REQUESTED");
}

static void set_error(MigrationReport *report, MigrationResultCode result, const char *reason, int error_number)
{
    report->result = result;
    report->errno_value = error_number;
    snprintf(report->error_reason, sizeof(report->error_reason), "%s", reason != NULL ? reason : "");
}

static bool proc_entry_exists(pid_t pid)
{
    char path[128];

    if (pid <= 0 || snprintf(path, sizeof(path), "/proc/%ld", (long)pid) >= (int)sizeof(path))
        return false;
    return access(path, F_OK) == 0;
}

static MigrationResultCode verify_process(pid_t pid, MigrationReport *report)
{
    if (!proc_entry_exists(pid)) {
        set_error(report, MIGRATION_TARGET_GONE, "process no longer exists", ESRCH);
        return report->result;
    }
    return MIGRATION_SUCCESS;
}

static MigrationResultCode verify_thread(const MigrationRequest *request, MigrationReport *report)
{
    char tid_path[128];
    char task_path[160];
    MigrationResultCode result;

    result = verify_process(request->pid, report);
    if (result != MIGRATION_SUCCESS)
        return result;
    if (request->tid <= 0) {
        set_error(report, MIGRATION_INVALID_TARGET, "thread ID is required", EINVAL);
        return report->result;
    }
    if (snprintf(tid_path, sizeof(tid_path), "/proc/%ld", (long)request->tid) >= (int)sizeof(tid_path) ||
        access(tid_path, F_OK) != 0) {
        set_error(report, MIGRATION_TARGET_GONE, "thread no longer exists", ESRCH);
        return report->result;
    }
    if (snprintf(task_path, sizeof(task_path), "/proc/%ld/task/%ld", (long)request->pid, (long)request->tid) >= (int)sizeof(task_path) ||
        access(task_path, F_OK) != 0) {
        set_error(report, MIGRATION_INVALID_TARGET, "thread does not belong to process", ESRCH);
        return report->result;
    }
    return MIGRATION_SUCCESS;
}

static bool node_exists(int node)
{
    char path[128];
    struct stat status;

    if (node < 0 || snprintf(path, sizeof(path), "/sys/devices/system/node/node%d", node) >= (int)sizeof(path))
        return false;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static MigrationResultCode verify_nodes(const MigrationRequest *request, MigrationReport *report)
{
    if (!request->numa_nodes_available || request->source_numa_node < 0 || request->destination_numa_node < 0) {
        set_error(report, MIGRATION_INSUFFICIENT_INFORMATION, "NUMA node metadata unavailable", 0);
        return report->result;
    }
    if (!node_exists(request->source_numa_node)) {
        set_error(report, MIGRATION_INVALID_SOURCE, "source NUMA node does not exist", ENODEV);
        return report->result;
    }
    if (!node_exists(request->destination_numa_node)) {
        set_error(report, MIGRATION_INVALID_DESTINATION, "destination NUMA node does not exist", ENODEV);
        return report->result;
    }
    return MIGRATION_SUCCESS;
}

static MigrationResultCode map_errno(int error_number, bool destination_error)
{
    switch (error_number) {
    case ESRCH: return MIGRATION_TARGET_GONE;
    case EPERM:
    case EACCES: return MIGRATION_PERMISSION_DENIED;
    case EINVAL: return destination_error ? MIGRATION_INVALID_DESTINATION : MIGRATION_INVALID_SOURCE;
    case ENODEV: return MIGRATION_INVALID_DESTINATION;
    case ENOSYS: return MIGRATION_UNSUPPORTED;
    default: return MIGRATION_SYSTEM_ERROR;
    }
}

static int affinity_to_string(const cpu_set_t *set, char *buffer, size_t size)
{
    size_t used = 0;
    bool first = true;

    if (size == 0)
        return -1;
    buffer[0] = '\0';
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        int written;

        if (!CPU_ISSET(cpu, set))
            continue;
        written = snprintf(buffer + used, size - used, "%s%d", first ? "" : ",", cpu);
        if (written < 0 || (size_t)written >= size - used)
            return -1;
        used += (size_t)written;
        first = false;
    }
    return first ? -1 : 0;
}

static MigrationResultCode build_requested_affinity(const MigrationRequest *request,
                                                    cpu_set_t *requested,
                                                    MigrationReport *report)
{
    cpu_set_t online;

    CPU_ZERO(requested);
    if (sched_getaffinity(0, sizeof(online), &online) != 0) {
        set_error(report, map_errno(errno, true), "cannot inspect available CPUs", errno);
        return report->result;
    }
    if (request->requested_cpu_set_available) {
        if (CPU_COUNT(&request->requested_cpu_set) == 0) {
            set_error(report, MIGRATION_INVALID_DESTINATION, "requested CPU set is empty", EINVAL);
            return report->result;
        }
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
            if (CPU_ISSET(cpu, &request->requested_cpu_set) && !CPU_ISSET(cpu, &online)) {
                set_error(report, MIGRATION_INVALID_DESTINATION, "requested CPU is unavailable", EINVAL);
                return report->result;
            }
        *requested = request->requested_cpu_set;
    } else {
        if (request->destination_cpu < 0 || request->destination_cpu >= CPU_SETSIZE ||
            !CPU_ISSET(request->destination_cpu, &online)) {
            set_error(report, MIGRATION_INVALID_DESTINATION, "destination CPU is unavailable", EINVAL);
            return report->result;
        }
        CPU_SET(request->destination_cpu, requested);
    }
    if (affinity_to_string(requested, report->requested_affinity, sizeof(report->requested_affinity)) != 0) {
        set_error(report, MIGRATION_INVALID_DESTINATION, "requested CPU set cannot be recorded", EINVAL);
        return report->result;
    }
    return MIGRATION_SUCCESS;
}

static MigrationResultCode execute_thread_migration(const MigrationRequest *request, MigrationReport *report)
{
    cpu_set_t old_affinity;
    cpu_set_t requested;
    cpu_set_t new_affinity;
    MigrationResultCode result;

    result = verify_thread(request, report);
    if (result != MIGRATION_SUCCESS)
        return result;
    result = build_requested_affinity(request, &requested, report);
    if (result != MIGRATION_SUCCESS)
        return result;
    if (sched_getaffinity(request->tid, sizeof(old_affinity), &old_affinity) != 0) {
        set_error(report, map_errno(errno, false), "cannot read current thread affinity", errno);
        return report->result;
    }
    if (affinity_to_string(&old_affinity, report->old_affinity, sizeof(report->old_affinity)) != 0) {
        set_error(report, MIGRATION_SYSTEM_ERROR, "current CPU affinity is empty", EINVAL);
        return report->result;
    }
    report->pages_requested = 1;
    if (CPU_EQUAL(&old_affinity, &requested)) {
        snprintf(report->verification_status, sizeof(report->verification_status), "NOT_REQUIRED");
        report->pages_requested = 0;
        return MIGRATION_NO_MIGRATION_REQUIRED;
    }
    if (sched_setaffinity(request->tid, sizeof(requested), &requested) != 0) {
        set_error(report, map_errno(errno, true), "sched_setaffinity failed", errno);
        return report->result;
    }
    report->pages_attempted = 1;
    if (!active_config.verification_enabled) {
        snprintf(report->verification_status, sizeof(report->verification_status), "UNAVAILABLE");
        report->pages_failed = 1;
        set_error(report, MIGRATION_VERIFICATION_UNAVAILABLE, "affinity verification disabled", 0);
        return report->result;
    }
    if (sched_getaffinity(request->tid, sizeof(new_affinity), &new_affinity) != 0) {
        snprintf(report->verification_status, sizeof(report->verification_status), "UNAVAILABLE");
        report->pages_failed = 1;
        set_error(report, MIGRATION_VERIFICATION_UNAVAILABLE, "cannot verify new thread affinity", errno);
        return report->result;
    }
    if (!CPU_EQUAL(&new_affinity, &requested)) {
        snprintf(report->verification_status, sizeof(report->verification_status), "FAILED");
        report->pages_failed = 1;
        set_error(report, MIGRATION_SYSTEM_ERROR, "new affinity differs from requested affinity", EIO);
        return report->result;
    }
    if (affinity_to_string(&new_affinity, report->new_affinity, sizeof(report->new_affinity)) != 0) {
        set_error(report, MIGRATION_SYSTEM_ERROR, "new CPU affinity cannot be recorded", EINVAL);
        return report->result;
    }
    snprintf(report->verification_status, sizeof(report->verification_status), "VERIFIED");
    report->pages_migrated = 1;
    return MIGRATION_SUCCESS;
}

static MigrationResultCode query_page_nodes(pid_t pid, void **pages, size_t page_count,
                                            int *status, MigrationReport *report)
{
#ifdef SYS_move_pages
    long result = syscall(SYS_move_pages, pid, page_count, pages, NULL, status, 0);

    if (result < 0) {
        int error_number = errno;

        set_error(report, map_errno(error_number, false), "move_pages query failed", error_number);
        return report->result;
    }
    return MIGRATION_SUCCESS;
#else
    (void)pid;
    (void)pages;
    (void)page_count;
    (void)status;
    set_error(report, MIGRATION_UNSUPPORTED, "move_pages is unavailable", ENOSYS);
    return report->result;
#endif
}

static MigrationResultCode execute_memory_migration(const MigrationRequest *request, MigrationReport *report)
{
    int *nodes = NULL;
    int *status = NULL;
    long result;
    long page_size;
    MigrationResultCode verification;

    verification = verify_process(request->pid, report);
    if (verification != MIGRATION_SUCCESS)
        return verification;
    verification = verify_nodes(request, report);
    if (verification != MIGRATION_SUCCESS)
        return verification;
    if (request->memory_locked || request->memory_pinned) {
        set_error(report, MIGRATION_INVALID_SOURCE, "memory is locked or pinned", EPERM);
        return report->result;
    }
    report->pages_requested = request->page_count;
    if (request->source_numa_node == request->destination_numa_node &&
        !request->explicit_placement_required && request->page_count == 0) {
        snprintf(report->verification_status, sizeof(report->verification_status), "NOT_REQUIRED");
        return MIGRATION_NO_MIGRATION_REQUIRED;
    }
    if (!request->page_metadata_available || !request->memory_region_verified ||
        request->pages == NULL || request->page_count == 0) {
        set_error(report, MIGRATION_INSUFFICIENT_INFORMATION, "verified page or region metadata is required", 0);
        return report->result;
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        set_error(report, MIGRATION_UNSUPPORTED, "page size is unavailable", ENOSYS);
        return report->result;
    }
    for (size_t index = 0; index < request->page_count; index++)
        if (request->pages[index] == NULL || (uintptr_t)request->pages[index] % (uintptr_t)page_size != 0) {
            set_error(report, MIGRATION_INVALID_SOURCE, "page address is invalid or unaligned", EINVAL);
            return report->result;
        }
    status = calloc(request->page_count, sizeof(*status));
    nodes = calloc(request->page_count, sizeof(*nodes));
    if (status == NULL || nodes == NULL) {
        free(status);
        free(nodes);
        set_error(report, MIGRATION_SYSTEM_ERROR, "cannot allocate migration metadata", ENOMEM);
        return report->result;
    }
    verification = query_page_nodes(request->pid, request->pages, request->page_count, status, report);
    if (verification != MIGRATION_SUCCESS)
        goto cleanup;
    for (size_t index = 0; index < request->page_count; index++)
        if (status[index] < 0 || status[index] != request->source_numa_node) {
            set_error(report, MIGRATION_INVALID_SOURCE, "page is not owned by the source node", EFAULT);
            goto cleanup;
        }
    if (request->source_numa_node == request->destination_numa_node) {
        snprintf(report->verification_status, sizeof(report->verification_status), "NOT_REQUIRED");
        verification = MIGRATION_NO_MIGRATION_REQUIRED;
        goto cleanup;
    }
#ifdef SYS_move_pages
    for (size_t index = 0; index < request->page_count; index++)
        nodes[index] = request->destination_numa_node;
    errno = 0;
    result = syscall(SYS_move_pages, request->pid, request->page_count, request->pages,
                     nodes, status, MPOL_MF_MOVE);
    report->pages_attempted = request->page_count;
    if (result < 0) {
        int error_number = errno;

        set_error(report, map_errno(error_number, true), "move_pages migration failed", error_number);
        for (size_t index = 0; index < request->page_count; index++)
            if (status[index] >= 0 && status[index] == request->destination_numa_node)
                report->pages_migrated++;
        report->pages_failed = report->pages_attempted - report->pages_migrated;
        if (report->pages_migrated > 0)
            report->result = MIGRATION_PARTIAL_SUCCESS;
        goto cleanup;
    }
    for (size_t index = 0; index < request->page_count; index++) {
        if (status[index] >= 0 && status[index] == request->destination_numa_node)
            report->pages_migrated++;
        else
            report->pages_failed++;
    }
    if (report->pages_migrated == 0) {
        set_error(report, MIGRATION_SYSTEM_ERROR, "no pages were migrated", EIO);
        goto cleanup;
    }
    if (report->pages_failed > 0) {
        report->result = MIGRATION_PARTIAL_SUCCESS;
        snprintf(report->error_reason, sizeof(report->error_reason), "some pages failed to migrate");
        goto cleanup;
    }
    if (!active_config.verification_enabled) {
        snprintf(report->verification_status, sizeof(report->verification_status), "UNAVAILABLE");
        set_error(report, MIGRATION_VERIFICATION_UNAVAILABLE, "NUMA verification disabled", 0);
        goto cleanup;
    }
    memset(status, 0, request->page_count * sizeof(*status));
    verification = query_page_nodes(request->pid, request->pages, request->page_count, status, report);
    if (verification != MIGRATION_SUCCESS) {
        snprintf(report->verification_status, sizeof(report->verification_status), "UNAVAILABLE");
        report->result = MIGRATION_VERIFICATION_UNAVAILABLE;
        goto cleanup;
    }
    for (size_t index = 0; index < request->page_count; index++)
        if (status[index] != request->destination_numa_node) {
            snprintf(report->verification_status, sizeof(report->verification_status), "FAILED");
            set_error(report, MIGRATION_SYSTEM_ERROR, "page placement verification failed", EIO);
            goto cleanup;
        }
    snprintf(report->verification_status, sizeof(report->verification_status), "VERIFIED");
    report->result = MIGRATION_SUCCESS;
#else
    set_error(report, MIGRATION_UNSUPPORTED, "move_pages is unavailable", ENOSYS);
#endif

cleanup:
    free(status);
    free(nodes);
    return report->result;
}

static void mark_cooldown(const MigrationRequest *request)
{
    size_t free_slot = MAX_COOLDOWN_ENTRIES;

    if (active_config.cooldown_ms == 0)
        return;
    pthread_mutex_lock(&execution_mutex);
    for (size_t index = 0; index < MAX_COOLDOWN_ENTRIES; index++) {
        if (cooldown_entries[index].in_use && same_key(request, cooldown_entries[index].pid,
                                                       cooldown_entries[index].tid,
                                                       cooldown_entries[index].entity_id)) {
            clock_gettime(CLOCK_MONOTONIC, &cooldown_entries[index].completed_at);
            pthread_mutex_unlock(&execution_mutex);
            return;
        }
        if (!cooldown_entries[index].in_use && free_slot == MAX_COOLDOWN_ENTRIES)
            free_slot = index;
    }
    if (free_slot != MAX_COOLDOWN_ENTRIES) {
        cooldown_entries[free_slot].in_use = true;
        cooldown_entries[free_slot].pid = request->pid;
        cooldown_entries[free_slot].tid = request->tid;
        snprintf(cooldown_entries[free_slot].entity_id, sizeof(cooldown_entries[free_slot].entity_id), "%s",
                 request->phase5_decision.entity_id);
        clock_gettime(CLOCK_MONOTONIC, &cooldown_entries[free_slot].completed_at);
    }
    pthread_mutex_unlock(&execution_mutex);
}

static bool cooldown_active(const MigrationRequest *request)
{
    struct timespec now;
    bool active = false;

    if (active_config.cooldown_ms == 0)
        return false;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&execution_mutex);
    for (size_t index = 0; index < MAX_COOLDOWN_ENTRIES; index++) {
        long long elapsed_ms;

        if (!cooldown_entries[index].in_use || !same_key(request, cooldown_entries[index].pid,
                                                         cooldown_entries[index].tid,
                                                         cooldown_entries[index].entity_id))
            continue;
        elapsed_ms = (long long)(now.tv_sec - cooldown_entries[index].completed_at.tv_sec) * 1000LL +
                     (long long)(now.tv_nsec - cooldown_entries[index].completed_at.tv_nsec) / 1000000LL;
        active = elapsed_ms < (long long)active_config.cooldown_ms;
        if (!active)
            memset(&cooldown_entries[index], 0, sizeof(cooldown_entries[index]));
        break;
    }
    pthread_mutex_unlock(&execution_mutex);
    return active;
}

MigrationResultCode Migration_Execute(const MigrationRequest *request, MigrationReport *report)
{
    double start;
    bool locked = false;
    MigrationResultCode result;

    if (report == NULL)
        return MIGRATION_SYSTEM_ERROR;
    memset(report, 0, sizeof(*report));
    if (!initialized || request == NULL) {
        set_error(report, MIGRATION_SYSTEM_ERROR, "migration module is not initialized", EINVAL);
        return report->result;
    }
    report_init(request, report);
    start = monotonic_seconds();
    if (request->phase5_decision.action != VALIDATION_ACTION_NO_MIGRATION &&
        request->phase5_decision.action != VALIDATION_ACTION_INSUFFICIENT &&
        (request->pid <= 0 || (request->phase5_decision.pid > 0 &&
                               request->phase5_decision.pid != (long)request->pid))) {
        set_error(report, MIGRATION_INVALID_TARGET, "request PID does not match Phase 5 target", EINVAL);
        goto finish;
    }
    if (request->phase5_decision.action == VALIDATION_ACTION_NO_MIGRATION) {
        set_error(report, MIGRATION_NO_ACTION, "Phase 5 selected NO_MIGRATION", 0);
        goto finish;
    }
    if (request->phase5_decision.action == VALIDATION_ACTION_INSUFFICIENT) {
        set_error(report, MIGRATION_INSUFFICIENT_INFORMATION, "Phase 5 returned INSUFFICIENT_DECISION_SIGNAL", 0);
        goto finish;
    }
    if (request->phase5_decision.action != VALIDATION_ACTION_MOVE_MEMORY &&
        request->phase5_decision.action != VALIDATION_ACTION_MOVE_THREAD) {
        set_error(report, MIGRATION_NOT_AUTHORIZED, "unsupported Phase 5 action", EINVAL);
        goto finish;
    }
    if (strcmp(request->phase6_validation.final_decision, "APPROVED") != 0) {
        set_error(report, MIGRATION_NOT_AUTHORIZED, "Phase 6 did not approve migration", 0);
        goto finish;
    }
    if (request->start_time_ticks_available &&
        !current_start_time_matches(request->pid, request->start_time_ticks)) {
        set_error(report, MIGRATION_TARGET_GONE, "PID start time no longer matches runtime identity", ESRCH);
        goto finish;
    }
    if (!Migration_AcquireExecutionLock(request)) {
        set_error(report, MIGRATION_ALREADY_IN_PROGRESS, "conflicting migration is already in progress", EBUSY);
        goto finish;
    }
    locked = true;
    if (cooldown_active(request)) {
        set_error(report, MIGRATION_ALREADY_IN_PROGRESS, "migration cooldown is active", EAGAIN);
        goto finish;
    }
    if (request->phase5_decision.action == VALIDATION_ACTION_MOVE_THREAD)
        result = execute_thread_migration(request, report);
    else
        result = execute_memory_migration(request, report);
    report->result = result;
    if (result == MIGRATION_SUCCESS || result == MIGRATION_PARTIAL_SUCCESS)
        mark_cooldown(request);

finish:
    if (locked)
        Migration_ReleaseExecutionLock(request);
    report->execution_time_ms = (monotonic_seconds() - start) * 1000.0;
    if (!migration_log_report(request, report) && report->error_reason[0] == '\0')
        snprintf(report->error_reason, sizeof(report->error_reason), "migration result logging failed");
    return report->result;
}

bool Migration_CleanupHistory(void)
{
    return initialized && migration_log_cleanup();
}
