#define _GNU_SOURCE

#include "benchmark_placement.h"
#include "memory_migration_transaction.h"
#include "migration.h"
#include "page_candidate_provider.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define P4C_PAGE_COUNT 4096U
#define P4C_APP_ID "p4c-migration-cost"
#define P4C_PROVENANCE "controlled_calibration"

typedef struct {
    int stage;
    int status;
    uint64_t start_time_ticks;
    uint64_t client_generation;
    PageCandidateResponseReason response_reason;
} child_message_t;

enum { CHILD_REGISTER_READY = 1, CHILD_REGISTRATION_RESULT = 2 };

static uint64_t start_ticks(pid_t pid)
{
    char path[64], line[4096], *cursor, *save = NULL;
    FILE *file;
    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path) ||
        (file = fopen(path, "r")) == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) fclose(file);
        return 0;
    }
    fclose(file);
    cursor = strrchr(line, ')');
    if (cursor == NULL) return 0;
    for (int field = 3; field <= 22; field++) {
        char *token = strtok_r(field == 3 ? cursor + 2 : NULL, " ", &save);
        if (token == NULL) return 0;
        if (field == 22) return strtoull(token, NULL, 10);
    }
    return 0;
}

static void child_workload(const char *socket_path, int ready_fd, int release_fd,
                           const benchmark_placement_topology_t *topology, size_t bytes)
{
    child_message_t ready = {.status = 1};
    void *region = MAP_FAILED;
    long page_size = sysconf(_SC_PAGESIZE);
    bool ready_sent = false;

    if (page_size <= 0 || bytes != P4C_PAGE_COUNT * (size_t)page_size) goto done;
    region = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) goto done;
    benchmark_placement_evidence_t placement;
    if (benchmark_placement_prepare(BENCHMARK_PLACEMENT_LOCAL, topology, region, bytes, &placement) != 0 ||
        strcmp(placement.verification_status, "PASS") != 0 || placement.total_pages != P4C_PAGE_COUNT ||
        placement.expected_node_pages != P4C_PAGE_COUNT || !placement.memory_policy_restored) {
        ready.status = 2;
        goto done;
    }
    ready.start_time_ticks = start_ticks(getpid());
    if (ready.start_time_ticks == 0) goto done;
    PageCandidateWireMessage message = {0};
    PageCandidateResponseReason reason = PAGE_CANDIDATE_REASON_MALFORMED;
    message.version = PAGE_CANDIDATE_WIRE_VERSION;
    message.operation = PAGE_CANDIDATE_WIRE_REGISTER;
    message.pid = getpid();
    message.start_time_ticks = ready.start_time_ticks;
    message.region_start = (uintptr_t)region;
    message.region_length = bytes;
    message.page_size = (size_t)page_size;
    message.client_generation = 1;
    snprintf(message.app_id, sizeof(message.app_id), "%s", P4C_APP_ID);
    snprintf(message.provenance, sizeof(message.provenance), "%s", P4C_PROVENANCE);
    ready.stage = CHILD_REGISTER_READY;
    ready.client_generation = message.client_generation;
    if (write(ready_fd, &ready, sizeof(ready)) != sizeof(ready)) goto done;
    ready_sent = true;
    ready.stage = CHILD_REGISTRATION_RESULT;
    ready.status = page_candidate_provider_send_wait(socket_path, &message, 2000, &reason, NULL) ==
                       PAGE_CANDIDATE_STATUS_ACCEPTED && reason == PAGE_CANDIDATE_REASON_ACCEPTED ? 0 : 3;
    ready.response_reason = reason;
done:
    if (ready.status == 2 && ready.stage == 0)
        ready.stage = CHILD_REGISTER_READY;
    if (!ready_sent || ready.stage == CHILD_REGISTRATION_RESULT)
        (void)write(ready_fd, &ready, sizeof(ready));
    if (ready.status == 0) {
        char release;
        (void)read(release_fd, &release, 1);
        PageCandidateWireMessage message = {.version = PAGE_CANDIDATE_WIRE_VERSION,
            .operation = PAGE_CANDIDATE_WIRE_UNREGISTER, .pid = getpid(),
            .start_time_ticks = ready.start_time_ticks, .region_start = (uintptr_t)region,
            .region_length = bytes, .page_size = (size_t)page_size, .client_generation = 2};
        snprintf(message.app_id, sizeof(message.app_id), "%s", P4C_APP_ID);
        snprintf(message.provenance, sizeof(message.provenance), "%s", P4C_PROVENANCE);
        (void)page_candidate_provider_send(socket_path, &message);
    }
    if (region != MAP_FAILED) munmap(region, bytes);
    _exit(ready.status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static void write_row(const char *path, const char *run_id, const char *warmup, size_t page_size,
                      const benchmark_placement_topology_t *topology, const MigrationReport *report,
                      bool valid, const char *reason)
{
    FILE *file = fopen(path, "a");
    if (file == NULL) return;
    fprintf(file, "%s,%s,%u,%zu,%zu,%zu,%.9g,%d,%d,%d,%zu,%s,%s\n", run_id, warmup,
            P4C_PAGE_COUNT, report->pages_attempted, report->pages_migrated, report->pages_failed,
            report->memory_operation_time_ms, topology->local_node, topology->remote_node,
            topology->numa_distance, page_size, valid ? "true" : "false", reason);
    fclose(file);
}

static bool poll_registration(page_candidate_provider_t *provider)
{
    for (unsigned attempt = 0; attempt < 200; attempt++) {
        if (page_candidate_provider_poll(provider)) return true;
        usleep(10000);
    }
    return false;
}

int main(int argc, char **argv)
{
    const char *output = NULL, *run_id = NULL, *warmup = NULL, *reason = "UNKNOWN";
    benchmark_placement_topology_t topology = {0};
    MigrationReport report = {0};
    long page_size = sysconf(_SC_PAGESIZE);
    bool valid = false, child_waiting = false, limited = false;
    int ready_pipe[2] = {-1, -1}, release_pipe[2] = {-1, -1};
    pid_t child = -1;
    page_candidate_provider_t *provider = NULL;
    MigrationRequest request = {0};
    char root[] = "/tmp/awavma-p4c-cost-XXXXXX", socket_path[108], results[PATH_MAX], history[PATH_MAX], log[PATH_MAX], state[PATH_MAX];
    child_message_t ready = {0};

    for (int index = 1; index + 1 < argc; index += 2) {
        if (strcmp(argv[index], "--output") == 0) output = argv[index + 1];
        else if (strcmp(argv[index], "--run-id") == 0) run_id = argv[index + 1];
        else if (strcmp(argv[index], "--warmup") == 0) warmup = argv[index + 1];
        else { fprintf(stderr, "usage: %s --output RAW_COST.csv --run-id ID --warmup true|false\n", argv[0]); return 2; }
    }
    if (output == NULL || run_id == NULL || warmup == NULL || (strcmp(warmup, "true") && strcmp(warmup, "false"))) {
        fprintf(stderr, "usage: %s --output RAW_COST.csv --run-id ID --warmup true|false\n", argv[0]);
        return 2;
    }
    if (page_size <= 0 || benchmark_placement_discover(&topology) != 0) {
        write_row(output, run_id, warmup, page_size > 0 ? (size_t)page_size : 0, &topology, &report,
                  false, "TOPOLOGY_ENV_LIMITED");
        fprintf(stderr, "TOPOLOGY_ENV_LIMITED\n");
        return 3;
    }
    if (mkdtemp(root) == NULL || pipe(ready_pipe) || pipe(release_pipe)) { reason = "SETUP_FAILED"; goto cleanup; }
    snprintf(socket_path, sizeof(socket_path), "%s/provider.sock", root);
    snprintf(results, sizeof(results), "%s/results.csv", root); snprintf(history, sizeof(history), "%s/history.csv", root);
    snprintf(log, sizeof(log), "%s/migration.log", root); snprintf(state, sizeof(state), "%s/state.csv", root);
    provider = page_candidate_provider_create();
    if (provider == NULL || !page_candidate_provider_start(provider, socket_path, 5000)) { reason = "PROVIDER_SETUP_FAILED"; goto cleanup; }
    child = fork();
    if (child == 0) { close(ready_pipe[0]); close(release_pipe[1]); child_workload(socket_path, ready_pipe[1], release_pipe[0], &topology, P4C_PAGE_COUNT * (size_t)page_size); }
    close(ready_pipe[1]); ready_pipe[1] = -1; close(release_pipe[0]); release_pipe[0] = -1;
    if (child < 0 || read(ready_pipe[0], &ready, sizeof(ready)) != sizeof(ready) || ready.stage != CHILD_REGISTER_READY) { reason = "CHILD_SETUP_FAILED"; goto cleanup; }
    if (ready.status == 2) { reason = "SOURCE_PLACEMENT_ENV_LIMITED"; limited = true; goto cleanup; }
    if (ready.status != 1 || !ready.start_time_ticks || !ready.client_generation || !poll_registration(provider) ||
        read(ready_pipe[0], &ready, sizeof(ready)) != sizeof(ready) || ready.stage != CHILD_REGISTRATION_RESULT ||
        ready.status != 0 || ready.response_reason != PAGE_CANDIDATE_REASON_ACCEPTED) { reason = "REGISTRATION_ACK_FAILED"; goto cleanup; }
    child_waiting = true;
    PageCandidateRegistrationStatus registration = {0};
    if (!page_candidate_provider_registration_status(provider, P4C_APP_ID, child, ready.start_time_ticks, &registration) ||
        !registration.accepted || registration.generation != ready.client_generation) { reason = "REGISTRATION_GENERATION_MISMATCH"; goto cleanup; }
    if (registration.registered_pages != P4C_PAGE_COUNT || registration.candidate_pages_per_request != P4C_PAGE_COUNT) { reason = "REGISTRATION_CANDIDATE_WINDOW_MISMATCH"; goto cleanup; }
    if (!page_candidate_provider_fill_request(provider, P4C_APP_ID, child, ready.start_time_ticks, topology.remote_node, &request) ||
        request.page_count != P4C_PAGE_COUNT || request.pages == NULL || !request.page_metadata_available ||
        !request.page_addresses_authoritative || !request.memory_region_verified) { reason = "AUTHORITATIVE_CANDIDATES_FAILED"; goto cleanup; }
    request.pid = child; request.tid = child; request.start_time_ticks = ready.start_time_ticks;
    request.start_time_ticks_available = true; request.numa_nodes_available = true;
    request.source_numa_node = topology.local_node; request.destination_numa_node = topology.remote_node;
    MigrationConfig config = {.results_path = results, .history_path = history, .log_path = log, .state_path = state,
        .history_max_records = 16, .history_max_days = 1.0, .history_decay_lambda = 0.1, .cleanup_interval = 1,
        .cooldown_ms = 0, .lock_timeout_ms = 0, .verification_enabled = true};
    if (!Migration_Init(&config)) { reason = "MIGRATION_SETUP_FAILED"; goto cleanup; }
    MemoryMigrationTransactionRequest transaction = {.request = &request, .mode = MEMORY_MIGRATION_TRANSACTION_CONTROLLED_CALIBRATION,
        .attempt_id = run_id, .expected_source_node = topology.local_node, .destination_node = topology.remote_node,
        .expected_page_count = P4C_PAGE_COUNT, .require_rollback = true};
    MemoryMigrationTransactionResult outcome;
    if (!memory_migration_transaction_execute(&transaction, &outcome)) { reason = "TRANSACTION_EXECUTION_FAILED"; goto cleanup; }
    report = outcome.migration_report;
    if (outcome.state_changed && !outcome.terminal_safe) reason = "UNSAFE_TERMINAL_STATE";
    else if (outcome.checkpoint_result != PAGE_CHECKPOINT_COMPLETE || !outcome.source_verified ||
             report.pages_requested != P4C_PAGE_COUNT || report.pages_attempted != P4C_PAGE_COUNT ||
             report.pages_migrated != P4C_PAGE_COUNT || report.pages_failed != 0 || !outcome.destination_verified ||
             report.memory_operation_time_ms <= 0 || outcome.rollback_summary.result != PAGE_ROLLBACK_COMPLETE ||
             !outcome.rollback_verified || !outcome.terminal_safe) reason = "INVALID_MIGRATION_OUTCOME";
    else { valid = true; reason = ""; }
cleanup:
    write_row(output, run_id, warmup, page_size > 0 ? (size_t)page_size : 0, &topology, &report, valid, reason);
    Migration_Shutdown(); page_candidate_provider_release_request(&request);
    if (release_pipe[1] >= 0 && child_waiting) (void)write(release_pipe[1], "x", 1);
    if (release_pipe[1] >= 0) close(release_pipe[1]);
    if (child > 0) { waitpid(child, NULL, 0); if (provider != NULL) (void)page_candidate_provider_poll(provider); }
    if (ready_pipe[0] >= 0) close(ready_pipe[0]);
    page_candidate_provider_destroy(provider); unlink(results); unlink(history); unlink(log); unlink(state); rmdir(root);
    if (!valid) fprintf(stderr, "%s\n", reason);
    return valid ? 0 : limited ? 3 : 1;
}
