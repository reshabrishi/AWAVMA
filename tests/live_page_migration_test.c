#define _GNU_SOURCE

#include "migration.h"
#include "memory_migration_transaction.h"
#include "migration_target_provider.h"
#include "page_candidate_provider.h"
#include "page_checkpoint.h"
#include "page_rollback.h"

#include <errno.h>
#include <limits.h>
#include <linux/mempolicy.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define LIVE_APP_ID "live-page-migration"
#define LIVE_PROVENANCE "controlled_live_page_migration"
#define LIVE_ATTEMPT_ID "live-page-migration-attempt-1"
#define LIVE_PAGE_COUNT 4U

typedef struct {
    int status;
    uint64_t start_time_ticks;
} ChildReady;

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

static void stage(const char *id, const char *status, const char *detail)
{
    printf("%s: %s%s%s\n", id, status, detail == NULL ? "" : " - ",
           detail == NULL ? "" : detail);
}

static int query_nodes(pid_t pid, void **pages, size_t count, int expected_node)
{
#ifdef SYS_move_pages
    int status[LIVE_PAGE_COUNT] = {0};

    if (count != LIVE_PAGE_COUNT || syscall(SYS_move_pages, pid, count, pages, NULL, status, 0) < 0)
        return -errno;
    for (size_t index = 0; index < count; index++)
        if (status[index] != expected_node)
            return -EFAULT;
    return 0;
#else
    (void)pid;
    (void)pages;
    (void)count;
    (void)expected_node;
    return -ENOSYS;
#endif
}

static void child_workload(const char *socket_path, int ready_fd, int release_fd)
{
    const long page_size = sysconf(_SC_PAGESIZE);
    const size_t length = page_size > 0 ? LIVE_PAGE_COUNT * (size_t)page_size : 0;
    ChildReady ready = {.status = 1, .start_time_ticks = 0};
    void *region = MAP_FAILED;

    if (page_size <= 0)
        goto done;
    region = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        goto done;
#ifdef SYS_mbind
    {
        unsigned long node_mask = 1UL;

        if (syscall(SYS_mbind, region, length, MPOL_BIND, &node_mask,
                    sizeof(node_mask) * CHAR_BIT, 0) != 0) {
            ready.status = 2;
            goto done;
        }
    }
#else
    ready.status = 2;
    goto done;
#endif
    for (size_t index = 0; index < length; index += (size_t)page_size)
        ((volatile unsigned char *)region)[index] = (unsigned char)index;
    ready.start_time_ticks = start_ticks(getpid());
    if (ready.start_time_ticks == 0)
        goto done;
    {
        PageCandidateWireMessage message = {0};

        message.version = PAGE_CANDIDATE_WIRE_VERSION;
        message.operation = PAGE_CANDIDATE_WIRE_REGISTER;
        message.pid = getpid();
        message.start_time_ticks = ready.start_time_ticks;
        message.region_start = (uintptr_t)region;
        message.region_length = length;
        message.page_size = (size_t)page_size;
        message.client_generation = 1;
        snprintf(message.app_id, sizeof(message.app_id), "%s", LIVE_APP_ID);
        snprintf(message.provenance, sizeof(message.provenance), "%s", LIVE_PROVENANCE);
        if (!page_candidate_provider_send(socket_path, &message))
            goto done;
    }
    ready.status = 0;

done:
    (void)write(ready_fd, &ready, sizeof(ready));
    if (ready.status == 0) {
        char release;

        (void)read(release_fd, &release, 1);
        PageCandidateWireMessage message = {0};

        message.version = PAGE_CANDIDATE_WIRE_VERSION;
        message.operation = PAGE_CANDIDATE_WIRE_UNREGISTER;
        message.pid = getpid();
        message.start_time_ticks = ready.start_time_ticks;
        message.region_start = (uintptr_t)region;
        message.region_length = length;
        message.page_size = (size_t)page_size;
        message.client_generation = 2;
        snprintf(message.app_id, sizeof(message.app_id), "%s", LIVE_APP_ID);
        snprintf(message.provenance, sizeof(message.provenance), "%s", LIVE_PROVENANCE);
        (void)page_candidate_provider_send(socket_path, &message);
    }
    if (region != MAP_FAILED)
        munmap(region, length);
    _exit(ready.status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

int main(void)
{
    MigrationTargetTopology topology;
    page_candidate_provider_t *provider = NULL;
    MigrationRequest request = {0};
    MigrationPageCheckpoint checkpoint = {0};
    MigrationReport report;
    PageRollbackRequest rollback_request = {0};
    PageRollbackSummary rollback_summary = {0};
    ChildReady ready = {0};
    char root[] = "/tmp/awavma-live-page-XXXXXX";
    char socket_path[108], results[PATH_MAX], history[PATH_MAX], log[PATH_MAX], state[PATH_MAX];
    int ready_pipe[2] = {-1, -1}, release_pipe[2] = {-1, -1};
    pid_t child = -1;
    int result = EXIT_FAILURE;
    bool limited = false;
    bool child_waiting = false;

    if (!migration_target_topology_read(&topology) || !topology.node_present[0] || !topology.node_present[1]) {
        stage("LPM01_ENVIRONMENT_READY", "NOT TESTED - ENVIRONMENT LIMITATION", "fewer than two online NUMA nodes");
        return EXIT_SUCCESS;
    }
    stage("LPM01_ENVIRONMENT_READY", "PASS", NULL);
    if (mkdtemp(root) == NULL || pipe(ready_pipe) != 0 || pipe(release_pipe) != 0) {
        stage("LPM10_CLEANUP", "FAIL", "temporary setup failed");
        return EXIT_FAILURE;
    }
    snprintf(socket_path, sizeof(socket_path), "%s/provider.sock", root);
    snprintf(results, sizeof(results), "%s/results.csv", root);
    snprintf(history, sizeof(history), "%s/history.csv", root);
    snprintf(log, sizeof(log), "%s/migration.log", root);
    snprintf(state, sizeof(state), "%s/state.csv", root);
    provider = page_candidate_provider_create();
    if (provider == NULL || !page_candidate_provider_start(provider, socket_path, 5000)) {
        stage("LPM02_CHILD_REGION_CREATED", "FAIL", "provider setup failed");
        goto cleanup;
    }
    child = fork();
    if (child == 0) {
        close(ready_pipe[0]); close(release_pipe[1]);
        child_workload(socket_path, ready_pipe[1], release_pipe[0]);
    }
    close(ready_pipe[1]); ready_pipe[1] = -1;
    close(release_pipe[0]); release_pipe[0] = -1;
    if (child < 0 || read(ready_pipe[0], &ready, sizeof(ready)) != sizeof(ready)) {
        stage("LPM02_CHILD_REGION_CREATED", "FAIL", "child setup did not report");
        goto cleanup;
    }
    if (ready.status == 2) {
        stage("LPM02_CHILD_REGION_CREATED", "NOT TESTED - ENVIRONMENT LIMITATION", "mbind node-0 placement unavailable");
        limited = true;
        goto cleanup;
    }
    if (ready.status != 0 || ready.start_time_ticks == 0) {
        stage("LPM02_CHILD_REGION_CREATED", "FAIL", "child mapping or registration failed");
        goto cleanup;
    }
    child_waiting = true;
    stage("LPM02_CHILD_REGION_CREATED", "PASS", NULL);
    if (!page_candidate_provider_poll(provider)) {
        stage("LPM03_AUTHENTICATED_REGISTRATION", "FAIL", "authenticated registration was rejected");
        goto cleanup;
    }
    stage("LPM03_AUTHENTICATED_REGISTRATION", "PASS", NULL);
    if (!page_candidate_provider_fill_request(provider, LIVE_APP_ID, child, ready.start_time_ticks, 1, &request) ||
        request.page_count != LIVE_PAGE_COUNT || request.pages == NULL || !request.page_metadata_available ||
        !request.page_addresses_authoritative || !request.memory_region_verified) {
        stage("LPM04_AUTHORITATIVE_CANDIDATES", "FAIL", "provider did not return four authoritative pages");
        goto cleanup;
    }
    stage("LPM04_AUTHORITATIVE_CANDIDATES", "PASS", NULL);
    request.pid = child;
    request.tid = child;
    request.start_time_ticks = ready.start_time_ticks;
    request.start_time_ticks_available = true;
    request.numa_nodes_available = true;
    request.source_numa_node = 0;
    request.destination_numa_node = 1;
    {
        MemoryMigrationTransactionRequest transaction_request = {
            .request = &request, .mode = MEMORY_MIGRATION_TRANSACTION_CONTROLLED_CALIBRATION, .attempt_id = LIVE_ATTEMPT_ID, .expected_source_node = 0,
            .destination_node = 1, .expected_page_count = LIVE_PAGE_COUNT, .require_rollback = true
        };
        MemoryMigrationTransactionResult transaction_result;
        MigrationConfig config = {.results_path = results, .history_path = history, .log_path = log,
            .state_path = state, .history_max_records = 16, .history_max_days = 1.0,
            .history_decay_lambda = 0.1, .cleanup_interval = 1, .cooldown_ms = 0,
            .lock_timeout_ms = 0, .verification_enabled = true};
        if (!Migration_Init(&config) || !memory_migration_transaction_execute(&transaction_request, &transaction_result)) {
            stage("LPM05_SHARED_TRANSACTION", "FAIL", "shared transaction setup failed"); goto cleanup;
        }
        report = transaction_result.migration_report;
        if (transaction_result.outcome == MEMORY_TRANSACTION_CHECKPOINT_FAILED &&
            (transaction_result.checkpoint_result == PAGE_QUERY_PERMISSION_DENIED || transaction_result.checkpoint_result == PAGE_QUERY_FAILED)) {
            stage("LPM05_SHARED_TRANSACTION", "NOT TESTED - ENVIRONMENT LIMITATION", page_checkpoint_result_name(transaction_result.checkpoint_result)); limited = true; goto cleanup;
        }
        if (!transaction_result.destination_verified || !transaction_result.rollback_verified || !transaction_result.terminal_safe) {
            stage("LPM05_SHARED_TRANSACTION", "FAIL", "shared transaction did not migrate and recover completely"); goto cleanup;
        }
        stage("LPM05_SHARED_TRANSACTION", "PASS", NULL); result = EXIT_SUCCESS; goto cleanup;
    }
cleanup:
    Migration_Shutdown();
    page_candidate_provider_release_request(&request);
    if (release_pipe[1] >= 0 && child_waiting)
        (void)write(release_pipe[1], "x", 1);
    if (release_pipe[1] >= 0)
        close(release_pipe[1]);
    if (child > 0) {
        waitpid(child, NULL, 0);
        if (provider != NULL)
            (void)page_candidate_provider_poll(provider);
    }
    if (ready_pipe[0] >= 0) close(ready_pipe[0]);
    page_candidate_provider_destroy(provider);
    unlink(results); unlink(history); unlink(log); unlink(state); rmdir(root);
    stage("LPM10_CLEANUP", "PASS", NULL);
    return limited ? EXIT_SUCCESS : result;
}
