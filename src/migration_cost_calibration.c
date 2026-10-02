#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <sched.h>

#include "migration.h"
#include "migration_target_provider.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WARMUP_REPETITIONS 20U
#define MEASURED_REPETITIONS 100U

static double monotonic_seconds(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static bool node_cpu(const MigrationTargetTopology *topology, int node, cpu_set_t *set)
{
    CPU_ZERO(set);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &topology->online_cpus) && topology->cpu_node[cpu] == node) {
            CPU_SET(cpu, set);
            return true;
        }
    return false;
}

int main(int argc, char **argv)
{
    MigrationTargetTopology topology;
    MigrationConfig config = {0};
    MigrationRequest request = {0};
    MigrationReport report;
    cpu_set_t source, target;
    char results[256], history[256], log[256], state[256];
    int source_node, target_node;
    double total = 0.0;

    if (argc != 4 || (source_node = atoi(argv[1])) < 0 || (target_node = atoi(argv[2])) < 0 ||
        source_node == target_node || !migration_target_topology_read(&topology) ||
        !node_cpu(&topology, source_node, &source) || !node_cpu(&topology, target_node, &target)) {
        fprintf(stderr, "Usage: %s SOURCE_NODE TARGET_NODE OUTPUT_ARTIFACT\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(results, sizeof(results), "/tmp/awavma-cost-%ld-results.csv", (long)getpid());
    snprintf(history, sizeof(history), "/tmp/awavma-cost-%ld-history.csv", (long)getpid());
    snprintf(log, sizeof(log), "/tmp/awavma-cost-%ld.log", (long)getpid());
    snprintf(state, sizeof(state), "/tmp/awavma-cost-%ld-state.csv", (long)getpid());
    config.results_path = results; config.history_path = history; config.log_path = log; config.state_path = state;
    config.history_max_records = 1000; config.history_max_days = 1.0; config.history_decay_lambda = 0.1;
    config.cleanup_interval = 1000; config.cooldown_ms = 0; config.verification_enabled = true;
    if (!Migration_Init(&config)) return EXIT_FAILURE;
    request.pid = getpid(); request.tid = getpid(); request.numa_nodes_available = true;
    request.source_numa_node = source_node; request.destination_numa_node = target_node;
    request.destination_cpu = -1; request.requested_cpu_set_available = true; request.requested_cpu_set = target;
    request.phase5_decision.action = VALIDATION_ACTION_MOVE_THREAD; request.phase5_decision.pid = getpid();
    snprintf(request.phase5_decision.migration_id, sizeof(request.phase5_decision.migration_id), "cost-calibration");
    snprintf(request.phase5_decision.app_id, sizeof(request.phase5_decision.app_id), "cost-calibration");
    snprintf(request.phase5_decision.entity_id, sizeof(request.phase5_decision.entity_id), "cost-calibration");
    snprintf(request.phase6_validation.final_decision, sizeof(request.phase6_validation.final_decision), "APPROVED");
    for (unsigned iteration = 0; iteration < WARMUP_REPETITIONS + MEASURED_REPETITIONS; iteration++) {
        double started;
        if (sched_setaffinity(0, sizeof(source), &source) != 0) break;
        started = monotonic_seconds();
        if (Migration_Execute(&request, &report) != MIGRATION_SUCCESS) break;
        if (iteration >= WARMUP_REPETITIONS) total += monotonic_seconds() - started;
        if (iteration + 1 == WARMUP_REPETITIONS + MEASURED_REPETITIONS) {
            FILE *output = fopen(argv[3], "w");
            if (output == NULL) break;
            fprintf(output, "schema_version,source_node,target_node,migration_cost_seconds,validation_status,provenance\n");
            fprintf(output, "1,%d,%d,%.9f,PASS,production_migration_execute_warmups20_reps100\n",
                    source_node, target_node, total / MEASURED_REPETITIONS);
            if (fclose(output) == 0) {
                Migration_Shutdown();
                unlink(results); unlink(history); unlink(log); unlink(state);
                return EXIT_SUCCESS;
            }
            break;
        }
    }
    Migration_Shutdown();
    unlink(results); unlink(history); unlink(log); unlink(state);
    return EXIT_FAILURE;
}
