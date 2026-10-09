#include "worker_evidence_provider.h"
#include "runtime_migration_metadata.h"

#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    worker_evidence_provider_t *provider;
    WorkerEvidenceWireMessage message = {0};
    worker_evidence_activity_t activity;
    char socket_path[108];
    char csv_path[128];
    pid_t child;
    int status;
    int ready[2];
    int release[2];

    assert(snprintf(socket_path, sizeof(socket_path), "/tmp/awavma-worker-evidence-%ld.sock", (long)getpid()) > 0);
    assert(snprintf(csv_path, sizeof(csv_path), "/tmp/awavma-worker-evidence-%ld.csv", (long)getpid()) > 0);
    provider = worker_evidence_provider_create();
    assert(provider != NULL && worker_evidence_provider_start(provider, socket_path));
    assert(pipe(ready) == 0 && pipe(release) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        uint64_t ticks;
        if (!runtime_read_start_time_ticks(getpid(), &ticks)) _exit(2);
        message.version = WORKER_EVIDENCE_WIRE_VERSION;
        message.operation = 1;
        message.pid = getpid();
        message.tid = getpid();
        message.process_start_time_ticks = ticks;
        message.worker_start_time_ticks = ticks;
        message.registration_generation = 1;
        message.evidence_generation = 1;
        message.interval_ms = 1000;
        message.registered_memory_load_operations = 41;
        snprintf(message.app_id, sizeof(message.app_id), "APP_%ld_%llu", (long)getpid(), (unsigned long long)ticks);
        if (!worker_evidence_provider_publish(socket_path, &message)) _exit(3);
        message.evidence_generation = 2; message.registered_memory_load_operations = 82;
        if (!worker_evidence_provider_publish(socket_path, &message) || write(ready[1], &message, sizeof(message)) != (ssize_t)sizeof(message)) _exit(3);
        if (read(release[0], &status, 1) != 1) _exit(4);
        _exit(0);
    }
    close(ready[1]); close(release[0]);
    assert(read(ready[0], &message, sizeof(message)) == (ssize_t)sizeof(message));
    for (unsigned tries = 0; tries < 20 && !worker_evidence_provider_poll(provider); tries++) usleep(10000);
    assert(worker_evidence_provider_candidate(provider, message.app_id, message.pid,
                                               message.process_start_time_ticks, message.tid,
                                               message.worker_start_time_ticks, 1, &activity));
    assert(activity.registered_memory_load_operations == 82 && activity.load_operations_delta == 41);
    assert(write(release[1], "x", 1) == 1);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(ready[0]); close(release[1]);
    assert(!worker_evidence_provider_candidate(provider, "wrong", getpid(), 1, getpid(), 1, 1, &activity));
    activity = (worker_evidence_activity_t){.pid = getpid(), .tid = getpid(), .registered_memory_load_operations = 0};
    snprintf(activity.app_id, sizeof(activity.app_id), "APP_TEST");
    assert(worker_evidence_activity_append(csv_path, &activity, false) == 0);
    worker_evidence_provider_destroy(provider);
    unlink(csv_path);
    return 0;
}
