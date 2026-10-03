#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#include "migration.h"
#include "migration_target_provider.h"
#include "runtime_migration_metadata.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WARMUP_REPETITIONS 20U
#define MEASURED_REPETITIONS 100U

typedef struct { pthread_mutex_t lock; pthread_cond_t ready; pthread_cond_t stop; pid_t tid; bool published; bool running; } Worker;

static double monotonic_seconds(void) { struct timespec value; clock_gettime(CLOCK_MONOTONIC, &value); return (double)value.tv_sec + (double)value.tv_nsec / 1e9; }

static void *worker_main(void *opaque)
{
    Worker *worker = opaque;
    pthread_mutex_lock(&worker->lock);
    worker->tid = (pid_t)syscall(SYS_gettid);
    worker->published = true;
    pthread_cond_signal(&worker->ready);
    while (worker->running) pthread_cond_wait(&worker->stop, &worker->lock);
    pthread_mutex_unlock(&worker->lock);
    return NULL;
}

static bool node_cpu(const MigrationTargetTopology *topology, const cpu_set_t *permitted, int node, cpu_set_t *set)
{
    CPU_ZERO(set);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &topology->online_cpus) && CPU_ISSET(cpu, permitted) && topology->cpu_node[cpu] == node) {
            CPU_SET(cpu, set);
            return true;
        }
    return false;
}

static void affinity_text(const cpu_set_t *set, char *text, size_t size)
{
    size_t used = 0;
    text[0] = '\0';
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, set)) {
        int written = snprintf(text + used, size - used, "%s%d", used == 0 ? "" : ",", cpu);
        if (written < 0 || (size_t)written >= size - used) return;
        used += (size_t)written;
    }
}

static bool verify_affinity(pid_t tid, const cpu_set_t *expected, cpu_set_t *actual)
{ return sched_getaffinity(tid, sizeof(*actual), actual) == 0 && CPU_EQUAL(actual, expected); }

static int first_cpu(const cpu_set_t *set)
{
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, set)) return cpu;
    return -1;
}

static int compare_double(const void *left, const void *right)
{ return (*(const double *)left > *(const double *)right) - (*(const double *)left < *(const double *)right); }

int main(int argc, char **argv)
{
    MigrationTargetTopology topology;
    RuntimeMigrationMetadata metadata;
    MigrationConfig config = {0};
    MigrationRequest request = {0};
    MigrationReport report = {0};
    Worker worker = { .lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER, .stop = PTHREAD_COND_INITIALIZER, .running = true };
    pthread_t thread;
    cpu_set_t source, target, actual;
    char results[256], history[256], log[256], state[256], raw[512], summary[512];
    double samples[MEASURED_REPETITIONS], sorted[MEASURED_REPETITIONS], sum = 0.0, variance = 0.0;
    int source_node, target_node;
    int status = EXIT_FAILURE;
    uint64_t start_time_ticks;

    if (argc != 4 || (source_node = atoi(argv[1])) != 1 || (target_node = atoi(argv[2])) != 0 ||
        !migration_target_topology_read(&topology) || !runtime_get_process_start_time_ticks(getpid(), &start_time_ticks) ||
        !runtime_get_migration_metadata(getpid(), start_time_ticks, &metadata) ||
        !metadata.permitted_cpu_set_available || !node_cpu(&topology, &metadata.permitted_cpu_set, source_node, &source) ||
        !node_cpu(&topology, &metadata.permitted_cpu_set, target_node, &target)) {
        fprintf(stderr, "Usage: %s SOURCE_NODE TARGET_NODE OUTPUT_ARTIFACT (effective cpuset must cover both nodes)\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (pthread_create(&thread, NULL, worker_main, &worker) != 0) return EXIT_FAILURE;
    pthread_mutex_lock(&worker.lock); while (!worker.published) pthread_cond_wait(&worker.ready, &worker.lock); pthread_mutex_unlock(&worker.lock);
    snprintf(results, sizeof(results), "/tmp/awavma-cost-%ld-results.csv", (long)getpid());
    snprintf(history, sizeof(history), "/tmp/awavma-cost-%ld-history.csv", (long)getpid());
    snprintf(log, sizeof(log), "/tmp/awavma-cost-%ld.log", (long)getpid());
    snprintf(state, sizeof(state), "/tmp/awavma-cost-%ld-state.csv", (long)getpid());
    config.results_path = results; config.history_path = history; config.log_path = log; config.state_path = state;
    config.history_max_records = 1000; config.history_max_days = 1.0; config.history_decay_lambda = 0.1; config.cleanup_interval = 1000; config.cooldown_ms = 0; config.verification_enabled = true;
    if (!Migration_Init(&config)) goto done;
    request.pid = getpid(); request.tid = worker.tid; request.numa_nodes_available = true;
    request.source_numa_node = source_node; request.destination_numa_node = target_node; request.destination_cpu = -1;
    request.requested_cpu_set_available = true; request.requested_cpu_set = target;
    request.permitted_cpu_set_available = true; request.permitted_cpu_set = metadata.permitted_cpu_set;
    request.phase5_decision.action = VALIDATION_ACTION_MOVE_THREAD; request.phase5_decision.pid = getpid();
    snprintf(request.phase5_decision.migration_id, sizeof(request.phase5_decision.migration_id), "cost-calibration");
    snprintf(request.phase5_decision.app_id, sizeof(request.phase5_decision.app_id), "cost-calibration");
    snprintf(request.phase5_decision.entity_id, sizeof(request.phase5_decision.entity_id), "cost-calibration");
    snprintf(request.phase6_validation.final_decision, sizeof(request.phase6_validation.final_decision), "APPROVED");
    for (unsigned iteration = 0; iteration < WARMUP_REPETITIONS + MEASURED_REPETITIONS; iteration++) {
        double started, elapsed;
        if (sched_setaffinity(worker.tid, sizeof(source), &source) != 0 || !verify_affinity(worker.tid, &source, &actual)) goto failed;
        started = monotonic_seconds();
        MigrationResultCode result = Migration_Execute(&request, &report);
        elapsed = monotonic_seconds() - started;
        if (result != MIGRATION_SUCCESS || report.mutation_state != MIGRATION_VERIFIED_SUCCESS || report.tid != worker.tid || !verify_affinity(worker.tid, &target, &actual)) {
failed:     { char old[256], wanted[256], newer[256]; affinity_text(&source, old, sizeof(old)); affinity_text(&target, wanted, sizeof(wanted)); affinity_text(&actual, newer, sizeof(newer));
              fprintf(stderr, "iteration=%u result=%s errno=%d reason=%s verification=%s old=%s requested=%s new=%s\n", iteration, MigrationResultName(report.result), report.errno_value, report.error_reason, report.verification_status, old, wanted, newer); }
            goto shutdown;
        }
        if (iteration >= WARMUP_REPETITIONS) samples[iteration - WARMUP_REPETITIONS] = elapsed;
    }
    for (unsigned index = 0; index < MEASURED_REPETITIONS; index++) sum += samples[index];
    memcpy(sorted, samples, sizeof(samples)); qsort(sorted, MEASURED_REPETITIONS, sizeof(sorted[0]), compare_double);
    for (unsigned index = 0; index < MEASURED_REPETITIONS; index++) { double delta = samples[index] - sum / MEASURED_REPETITIONS; variance += delta * delta; }
    snprintf(raw, sizeof(raw), "%s.raw.csv", argv[3]);
    FILE *output = fopen(raw, "w");
    if (output == NULL) goto shutdown;
    fprintf(output, "repetition,pid,tid,source_node,target_node,source_cpu,target_cpu,migration_cost_seconds,migration_result,verification_status\n");
    for (unsigned index = 0; index < MEASURED_REPETITIONS; index++) fprintf(output, "%u,%ld,%ld,%d,%d,%d,%d,%.9f,MIGRATION_SUCCESS,VERIFIED\n", index + 1, (long)getpid(), (long)worker.tid, source_node, target_node, first_cpu(&source), first_cpu(&target), samples[index]);
    if (fclose(output) != 0) goto shutdown;
    output = fopen(argv[3], "w");
    if (output == NULL) goto shutdown;
    struct utsname system; uname(&system);
    fprintf(output, "schema_version,source_node,target_node,migration_cost_seconds,validation_status,provenance\n");
    fprintf(output, "1,%d,%d,%.9f,PASS,production_migration_execute_worker_tid\n", source_node, target_node, sum / MEASURED_REPETITIONS);
    if (fclose(output) != 0) goto shutdown;
    snprintf(summary, sizeof(summary), "%s.summary.csv", argv[3]);
    output = fopen(summary, "w");
    if (output == NULL) goto shutdown;
    fprintf(output, "sample_count,mean,median,sample_standard_deviation,min,max,kernel,warmups,measured\n");
    fprintf(output, "%u,%.9f,%.9f,%.9f,%.9f,%.9f,%s,%u,%u\n", MEASURED_REPETITIONS, sum / MEASURED_REPETITIONS, (sorted[49] + sorted[50]) / 2.0, sqrt(variance / (MEASURED_REPETITIONS - 1)), sorted[0], sorted[99], system.release, WARMUP_REPETITIONS, MEASURED_REPETITIONS);
    if (fclose(output) == 0) status = EXIT_SUCCESS;
shutdown:
    Migration_Shutdown();
done:
    pthread_mutex_lock(&worker.lock); worker.running = false; pthread_cond_signal(&worker.stop); pthread_mutex_unlock(&worker.lock); pthread_join(thread, NULL);
    unlink(results); unlink(history); unlink(log); unlink(state);
    return status;
}
