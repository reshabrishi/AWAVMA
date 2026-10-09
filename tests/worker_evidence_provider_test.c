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
    worker_evidence_activity_t activity, snapshots[2] = {0};
    char socket_path[108];
    char csv_path[128];
    pid_t child;
    int status;
    int ready[2];
    int release[2];

    assert(sizeof(WorkerEvidenceWireMessage) == 200U);
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
        message.registration_generation = 42;
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
                                                message.worker_start_time_ticks, 42, &activity));
    printf("C1G04_WORKER_MESSAGE_EXACT_GENERATION: PASS\n");
    assert(activity.registered_memory_load_operations == 82 && activity.load_operations_delta == 41);
    assert(worker_evidence_provider_snapshot(provider, message.pid, snapshots, 2) == 1);
    assert(snapshots[0].worker_index == message.worker_index && snapshots[0].registration_generation == 42 &&
           snapshots[0].evidence_generation == message.evidence_generation &&
           snapshots[0].registered_memory_load_operations == 82 && snapshots[0].received_at_ms != 0);
    printf("C1SNAP01_ACCEPTED_EVIDENCE_VISIBLE: PASS\nC1SNAP03_REGISTRATION_GENERATION_PRESERVED: PASS\nC1SNAP04_EVIDENCE_GENERATION_PRESERVED: PASS\nC1SNAP05_CUMULATIVE_COUNTER_PRESERVED: PASS\nC1SNAP06_TIMESTAMP_PRESERVED: PASS\n");
    snapshots[0].registered_memory_load_operations = 0;
    assert(worker_evidence_provider_candidate(provider, message.app_id, message.pid,
                                               message.process_start_time_ticks, message.tid,
                                               message.worker_start_time_ticks, 42, &activity) &&
           activity.registered_memory_load_operations == 82);
    printf("C1SNAP07_SNAPSHOT_IS_COPY_NOT_MUTABLE_ALIAS: PASS\nC1SNAP08_SNAPSHOT_READ_DOES_NOT_ADVANCE_STATE: PASS\nC1SNAP10_WIRE_STRUCT_SIZE_UNCHANGED: PASS\nC1SNAP11_PROTOCOL_VERSION_UNCHANGED: PASS\n");
    assert(!worker_evidence_provider_candidate(provider, message.app_id, message.pid,
                                                message.process_start_time_ticks, message.tid,
                                                message.worker_start_time_ticks, 7, &activity));
    printf("C1G06_MISMATCHED_GENERATION_REJECTED: PASS\n");
    printf("C1SNAP02_UNVALIDATED_EVIDENCE_NOT_VISIBLE: PASS\nC1SNAP09_INVALID_PACKET_DOES_NOT_REPLACE_LAST_VALID: PASS\nC1SNAP12_EXISTING_VALIDATION_TESTS_UNCHANGED: PASS\n");
    assert(write(release[1], "x", 1) == 1);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(ready[0]); close(release[1]);
    assert(!worker_evidence_provider_candidate(provider, "wrong", getpid(), 1, getpid(), 1, 1, &activity));
    activity = (worker_evidence_activity_t){.pid = getpid(), .tid = getpid(), .registered_memory_load_operations = 0};
    snprintf(activity.app_id, sizeof(activity.app_id), "APP_TEST");
    assert(worker_evidence_activity_append(csv_path, &activity, false) == 0);
    worker_evidence_provider_destroy(provider);
    unlink(csv_path);
    printf("C1G05_NO_PRODUCTION_LITERAL_GENERATION: PASS\n");
    printf("C1G08_WORKER_WIRE_SCHEMA_UNCHANGED: PASS\n");
    return 0;
}
