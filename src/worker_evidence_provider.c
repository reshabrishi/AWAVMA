#define _POSIX_C_SOURCE 200809L
#include "worker_evidence_provider.h"

#include "runtime_migration_metadata.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct worker_evidence_provider { int socket_fd; pthread_mutex_t entries_mutex; char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)]; worker_evidence_activity_t entries[WORKER_EVIDENCE_MAX_WORKERS]; };


static uint64_t now_ms(void) { struct timespec t; return clock_gettime(CLOCK_MONOTONIC, &t) == 0 ? (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U : 0; }
static bool safe_text(const char *text, size_t size) { return text != NULL && memchr(text, '\0', size) != NULL && text[0] != '\0'; }
static bool task_ticks(pid_t pid, pid_t tid, uint64_t *ticks)
{
    char path[96], line[4096], *cursor, *save = NULL;
    FILE *file;
    if (ticks == NULL || snprintf(path, sizeof(path), "/proc/%ld/task/%ld/stat", (long)pid, (long)tid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) { if (file != NULL) fclose(file); return false; }
    fclose(file); cursor = strrchr(line, ')'); if (cursor == NULL || cursor[1] != ' ') return false; cursor += 2;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor : NULL, " ", &save);
        char *end = NULL;
        unsigned long long value;
        if (token == NULL) return false;
        if (field != 22) continue;
        errno = 0;
        value = strtoull(token, &end, 10);
        if (errno != 0 || end == token || (*end != '\0' && *end != '\n') || value == 0) return false;
        *ticks = (uint64_t)value;
        return true;
    }
    return false;
}

worker_evidence_provider_t *worker_evidence_provider_create(void) { worker_evidence_provider_t *p = calloc(1, sizeof(*p)); if (p != NULL && pthread_mutex_init(&p->entries_mutex, NULL) == 0) p->socket_fd = -1; else { free(p); p = NULL; } return p; }
void worker_evidence_provider_stop(worker_evidence_provider_t *p) { if (p == NULL) return; if (p->socket_fd >= 0) close(p->socket_fd); p->socket_fd = -1; if (p->socket_path[0] != '\0') unlink(p->socket_path); p->socket_path[0] = '\0'; }
void worker_evidence_provider_destroy(worker_evidence_provider_t *p) { if (p != NULL) { worker_evidence_provider_stop(p); pthread_mutex_destroy(&p->entries_mutex); } free(p); }
bool worker_evidence_provider_start(worker_evidence_provider_t *p, const char *path)
{
    struct sockaddr_un address = {0}; int flags;
    if (p == NULL || path == NULL || path[0] == '\0' || strlen(path) >= sizeof(address.sun_path)) return false;
    worker_evidence_provider_stop(p); p->socket_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0); if (p->socket_fd < 0) return false;
    address.sun_family = AF_UNIX; snprintf(address.sun_path, sizeof(address.sun_path), "%s", path); unlink(path);
    if (bind(p->socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 || chmod(path, S_IRUSR | S_IWUSR) != 0 ||
        listen(p->socket_fd, 32) != 0 || (flags = fcntl(p->socket_fd, F_GETFL, 0)) < 0 || fcntl(p->socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) { worker_evidence_provider_stop(p); return false; }
    snprintf(p->socket_path, sizeof(p->socket_path), "%s", path); return true;
}
bool worker_evidence_provider_poll(worker_evidence_provider_t *p)
{
    bool accepted = false;
    if (p == NULL || p->socket_fd < 0) return false;
    for (;;) { int client = accept(p->socket_fd, NULL, NULL); struct ucred credential; socklen_t credential_size = sizeof(credential); WorkerEvidenceWireMessage message = {0}; ssize_t received;
        if (client < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? accepted : false;
        received = recv(client, &message, sizeof(message), MSG_TRUNC);
        if (received == (ssize_t)sizeof(message) && message.version == WORKER_EVIDENCE_WIRE_VERSION && message.operation == 1U && message.pid > 0 && message.tid > 0 &&
            safe_text(message.app_id, sizeof(message.app_id)) && getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credential, &credential_size) == 0 && credential.uid == geteuid() && credential.pid == message.pid) {
            RuntimeMigrationMetadata metadata; uint64_t ticks = 0;
            if (runtime_get_migration_metadata(message.pid, message.process_start_time_ticks, &metadata) && metadata.process_exists && metadata.identity_match &&
                task_ticks(message.pid, message.tid, &ticks) && ticks == message.worker_start_time_ticks) {
                size_t index = WORKER_EVIDENCE_MAX_WORKERS;
                pthread_mutex_lock(&p->entries_mutex);
                for (size_t i = 0; i < WORKER_EVIDENCE_MAX_WORKERS; i++) if (p->entries[i].pid == message.pid && p->entries[i].tid == message.tid && p->entries[i].worker_start_time_ticks == ticks) { index = i; break; }
                if (index == WORKER_EVIDENCE_MAX_WORKERS) for (size_t i = 0; i < WORKER_EVIDENCE_MAX_WORKERS; i++) if (p->entries[i].pid == 0) { index = i; break; }
                if (index < WORKER_EVIDENCE_MAX_WORKERS && message.evidence_generation != 0 &&
                    message.interval_ms != 0 &&
                    (p->entries[index].pid == 0 ||
                     (message.evidence_generation == p->entries[index].evidence_generation + 1 &&
                      message.registered_memory_load_operations >= p->entries[index].registered_memory_load_operations))) {
                    worker_evidence_activity_t previous = p->entries[index];
                    p->entries[index] = (worker_evidence_activity_t){.pid = message.pid, .process_start_time_ticks = message.process_start_time_ticks, .tid = message.tid, .worker_start_time_ticks = ticks, .registration_generation = message.registration_generation, .evidence_generation = message.evidence_generation, .interval_ms = message.interval_ms, .worker_index = message.worker_index, .registered_memory_load_operations = message.registered_memory_load_operations, .load_operations_delta = previous.pid == 0 ? 0 : message.registered_memory_load_operations - previous.registered_memory_load_operations, .received_at_ms = now_ms()}; snprintf(p->entries[index].app_id, sizeof(p->entries[index].app_id), "%s", message.app_id); accepted = true; }
                pthread_mutex_unlock(&p->entries_mutex);
            }
        }
        close(client);
    }
}
bool worker_evidence_provider_publish(const char *path, const WorkerEvidenceWireMessage *message)
{
    struct sockaddr_un address = {0}; int fd;
    if (path == NULL || message == NULL || strlen(path) >= sizeof(address.sun_path)) return false;
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0); if (fd < 0) return false; address.sun_family = AF_UNIX; snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    bool ok = connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0 && send(fd, message, sizeof(*message), 0) == (ssize_t)sizeof(*message); close(fd); return ok;
}
bool worker_evidence_provider_candidate(worker_evidence_provider_t *p, const char *app, pid_t pid, uint64_t process_ticks, pid_t tid, uint64_t worker_ticks, uint64_t registration_generation, worker_evidence_activity_t *activity)
{
    if (activity != NULL) memset(activity, 0, sizeof(*activity));
    if (p == NULL || app == NULL || activity == NULL) return false;
    pthread_mutex_lock(&p->entries_mutex);
    for (size_t i = 0; i < WORKER_EVIDENCE_MAX_WORKERS; i++) if (p->entries[i].pid == pid && p->entries[i].process_start_time_ticks == process_ticks && p->entries[i].tid == tid && p->entries[i].worker_start_time_ticks == worker_ticks && p->entries[i].registration_generation == registration_generation && p->entries[i].load_operations_delta != 0 && now_ms() >= p->entries[i].received_at_ms && now_ms() - p->entries[i].received_at_ms <= WORKER_EVIDENCE_FRESHNESS_MS && strcmp(p->entries[i].app_id, app) == 0) { *activity = p->entries[i]; pthread_mutex_unlock(&p->entries_mutex); return true; }
    pthread_mutex_unlock(&p->entries_mutex);
    return false;
}
size_t worker_evidence_provider_snapshot(worker_evidence_provider_t *p, pid_t pid, worker_evidence_activity_t *out, size_t capacity)
{
    size_t count = 0;
    if (p == NULL || out == NULL) return 0;
    pthread_mutex_lock(&p->entries_mutex);
    for (size_t i = 0; i < WORKER_EVIDENCE_MAX_WORKERS && count < capacity; ++i)
        if (p->entries[i].pid == pid && p->entries[i].load_operations_delta != 0 && now_ms() >= p->entries[i].received_at_ms && now_ms() - p->entries[i].received_at_ms <= WORKER_EVIDENCE_FRESHNESS_MS)
            out[count++] = p->entries[i];
    pthread_mutex_unlock(&p->entries_mutex);
    return count;
}
int worker_evidence_activity_append(const char *path, const worker_evidence_activity_t *activity, bool matched)
{
    FILE *file; bool header;
    if (path == NULL || activity == NULL) return -1;
    file = fopen(path, "a+");
    if (file == NULL) return -1;
    header = fseek(file, 0, SEEK_END) != 0 || ftell(file) == 0;
    if (header) fprintf(file, "schema_version,received_at_ms,app_id,pid,process_start_time_ticks,candidate_tid,candidate_tid_start_time_ticks,worker_index,worker_evidence_generation,registration_generation,load_operations_total,load_operations_delta,interval_ms,load_rate_ops_per_ms,activity_evidence_valid,exact_candidate_match,provenance\n");
    fprintf(file, "1,%llu,%s,%ld,%llu,%ld,%llu,%u,%llu,%llu,%llu,%llu,%llu,%.9g,%s,%s,BENCHMARK_WORKER_COOPERATIVE\n", (unsigned long long)activity->received_at_ms, activity->app_id, (long)activity->pid, (unsigned long long)activity->process_start_time_ticks, (long)activity->tid, (unsigned long long)activity->worker_start_time_ticks, activity->worker_index, (unsigned long long)activity->evidence_generation, (unsigned long long)activity->registration_generation, (unsigned long long)activity->registered_memory_load_operations, (unsigned long long)activity->load_operations_delta, (unsigned long long)activity->interval_ms, activity->interval_ms == 0 ? 0.0 : (double)activity->load_operations_delta / activity->interval_ms, matched ? "true" : "false", matched ? "true" : "false");
    return fflush(file) == 0 && fclose(file) == 0 ? 0 : -1;
}
