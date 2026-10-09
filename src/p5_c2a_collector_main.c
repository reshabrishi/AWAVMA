#include "benchmark_placement.h"
#include "p5_c2a_collector.h"
#include "p5_thread_activity_calibration_io.h"
#include "worker_evidence_provider.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NUMA_BALANCING "/proc/sys/kernel/numa_balancing"
#define MAX_SAMPLES 65536U

typedef struct {
    bool dry_run, execute;
    const char *benchmark, *root, *local_id, *remote_id;
    unsigned workers, memory_mb, runs, duration_ms;
    uint64_t seed;
} Options;

static char saved_numa_balancing[32];
static bool numa_balancing_changed;
static volatile sig_atomic_t interrupted;

static void restore_numa_balancing(void)
{
    FILE *file;
    if (!numa_balancing_changed) return;
    file = fopen(NUMA_BALANCING, "w");
    if (file != NULL) { (void)fputs(saved_numa_balancing, file); (void)fclose(file); }
    numa_balancing_changed = false;
}

static void on_signal(int signal_number) { (void)signal_number; interrupted = 1; }

static int usage(const char *program)
{
    fprintf(stderr, "usage: %s [--dry-run|--execute] [--benchmark PATH] [--output-root PATH]\\n"
            "          [--local-id ID] [--remote-id ID] [--workers N] [--memory-mb N]\\n"
            "          [--runs N] [--duration-ms N] [--seed N]\\n", program);
    return 2;
}

static bool number(const char *text, uint64_t *value)
{
    char *end = NULL; unsigned long long parsed;
    if (text == NULL || !*text || *text == '-') return false;
    errno = 0; parsed = strtoull(text, &end, 10);
    if (errno || *end) return false;
    *value = parsed; return true;
}

static bool argument(int *index, int argc, char **argv, uint64_t *value)
{
    return ++*index < argc && number(argv[*index], value) && *value > 0;
}

static bool parse_options(int argc, char **argv, Options *options)
{
    *options = (Options){.benchmark = "./bin/benchmark", .root = "results", .local_id = "c2a3-local",
        .remote_id = "c2a3-remote", .workers = 1, .memory_mb = 64, .runs = 12, .duration_ms = 1000, .seed = 12345};
    for (int i = 1; i < argc; ++i) {
        uint64_t value;
        if (!strcmp(argv[i], "--dry-run")) options->dry_run = true;
        else if (!strcmp(argv[i], "--execute")) options->execute = true;
        else if ((!strcmp(argv[i], "--benchmark") || !strcmp(argv[i], "--benchmark-path")) && ++i < argc) options->benchmark = argv[i];
        else if (!strcmp(argv[i], "--output-root") && ++i < argc) options->root = argv[i];
        else if ((!strcmp(argv[i], "--local-id") || !strcmp(argv[i], "--local-calibration-id")) && ++i < argc) options->local_id = argv[i];
        else if ((!strcmp(argv[i], "--remote-id") || !strcmp(argv[i], "--remote-calibration-id")) && ++i < argc) options->remote_id = argv[i];
        else if ((!strcmp(argv[i], "--workers") || !strcmp(argv[i], "--worker-count")) && argument(&i, argc, argv, &value) && value <= 256) options->workers = (unsigned)value;
        else if (!strcmp(argv[i], "--memory-mb") && argument(&i, argc, argv, &value) && value <= UINT32_MAX) options->memory_mb = (unsigned)value;
        else if (!strcmp(argv[i], "--runs") && argument(&i, argc, argv, &value) && value <= 1000) options->runs = (unsigned)value;
        else if (!strcmp(argv[i], "--duration-ms") && argument(&i, argc, argv, &value) && value <= 3600000) options->duration_ms = (unsigned)value;
        else if (!strcmp(argv[i], "--seed") && argument(&i, argc, argv, &value)) options->seed = value;
        else return false;
    }
    return !(options->dry_run && options->execute) && p5_thread_activity_calibration_id_valid(options->local_id) &&
           p5_thread_activity_calibration_id_valid(options->remote_id) && strcmp(options->local_id, options->remote_id);
}

static bool disable_numa_balancing(void)
{
    FILE *file = fopen(NUMA_BALANCING, "r");
    if (file == NULL || fgets(saved_numa_balancing, sizeof(saved_numa_balancing), file) == NULL) { if (file) fclose(file); return false; }
    fclose(file);
    file = fopen(NUMA_BALANCING, "w");
    if (file == NULL) return false;
    numa_balancing_changed = true;
    if (fputs("0\n", file) == EOF || fclose(file) != 0) { restore_numa_balancing(); return false; }
    return true;
}

static void context(P5ThreadActivityContext *context, const Options *options, P5ThreadActivityPlacementMode placement)
{
    struct utsname name;
    memset(context, 0, sizeof(*context)); (void)uname(&name);
    context->schema_version = P5_THREAD_ACTIVITY_SCHEMA_VERSION;
    snprintf(context->architecture, sizeof(context->architecture), "%s", name.machine[0] ? name.machine : "unknown");
    snprintf(context->cpu_vendor, sizeof(context->cpu_vendor), "linux");
    snprintf(context->cpu_model_name, sizeof(context->cpu_model_name), "collector");
    context->online_cpu_count = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN); context->numa_node_count = 2;
    context->page_size = (uint64_t)sysconf(_SC_PAGESIZE); context->worker_count = options->workers;
    context->memory_mb = options->memory_mb; context->placement_mode = placement; context->benchmark_seed = options->seed;
    snprintf(context->metric_name, sizeof(context->metric_name), "%s", P5_THREAD_ACTIVITY_METRIC_NAME);
    snprintf(context->metric_unit, sizeof(context->metric_unit), "%s", P5_THREAD_ACTIVITY_METRIC_UNIT);
    snprintf(context->benchmark_definition_version, sizeof(context->benchmark_definition_version), "benchmark-random-intensity-v1");
    snprintf(context->intensity_profile_version, sizeof(context->intensity_profile_version), "%s", P5_C2A_COLLECTOR_MATRIX_A_VERSION);
    context->metric_version = P5_THREAD_ACTIVITY_METRIC_VERSION; context->low_intensity_percent = 10;
    context->mid_intensity_percent = 40; context->high_intensity_percent = 100; context->numa_balancing_state = 0;
    context->window_interval_count = P5_THREAD_ACTIVITY_WINDOW_INTERVALS;
    context->minimum_worker_run_units = P5_THREAD_ACTIVITY_MIN_WORKER_RUN_UNITS;
    (void)p5_thread_activity_hardware_fingerprint(context, context->hardware_fingerprint, sizeof(context->hardware_fingerprint));
}

static pid_t launch(const Options *o, const benchmark_placement_topology_t *t, P5ThreadActivityPlacementMode placement,
                    unsigned intensity, const char *socket, unsigned run)
{
    char workers[16], memory[16], duration[16], seed[32], thread_node[16], memory_node[16], evidence[512]; pid_t child = fork();
    if (child != 0) return child;
    snprintf(workers, sizeof(workers), "%u", o->workers); snprintf(memory, sizeof(memory), "%u", o->memory_mb);
    snprintf(duration, sizeof(duration), "%.3f", (double)o->duration_ms / 1000.0); snprintf(seed, sizeof(seed), "%llu", (unsigned long long)(o->seed + run));
    snprintf(thread_node, sizeof(thread_node), "%d", t->local_node); snprintf(memory_node, sizeof(memory_node), "%d", placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? t->local_node : t->remote_node);
    snprintf(evidence, sizeof(evidence), "%s/c2a3-%c-%u-placement.csv", o->root, placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? 'l' : 'r', run);
    char *const argv[] = {(char *)o->benchmark, "--threads", workers, "--memory", memory, "--duration", duration,
        "--pattern", "random", "--seed", seed, "--thread-node", thread_node, "--memory-node", memory_node,
        "--placement-mode", placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "local" : "remote",
        "--intensity-percent", (char [16]){0}, "--worker-evidence-socket", (char *)socket,
        "--placement-evidence", evidence, NULL};
    snprintf(argv[20], 16, "%u", intensity); execv(o->benchmark, argv); _exit(127);
}

static size_t collect_run(worker_evidence_provider_t *provider, pid_t child, P5ThreadActivityRawSample *samples, size_t count,
                          const P5ThreadActivityContext *context, const char *id, unsigned run, P5ThreadActivityProfile profile, unsigned intensity,
                          bool *child_ok)
{
    uint64_t generations[WORKER_EVIDENCE_MAX_WORKERS] = {0}; int status; bool alive = true;
    *child_ok = false;
    while (alive && !interrupted) {
        worker_evidence_activity_t activities[WORKER_EVIDENCE_MAX_WORKERS]; size_t n;
        (void)worker_evidence_provider_poll(provider); n = worker_evidence_provider_snapshot(provider, child, activities, WORKER_EVIDENCE_MAX_WORKERS);
        for (size_t i = 0; i < n && count < MAX_SAMPLES; ++i) if (activities[i].worker_index < WORKER_EVIDENCE_MAX_WORKERS && activities[i].evidence_generation > generations[activities[i].worker_index]) {
            P5ThreadActivityRawSample *sample = &samples[count++]; P5C2AEvidenceLabel label; uint64_t delta;
            generations[activities[i].worker_index] = activities[i].evidence_generation;
            if (!p5_c2a_collector_label_delta(activities[i].evidence_generation == 1 ? 0 : activities[i].registered_memory_load_operations - activities[i].load_operations_delta,
                                              activities[i].registered_memory_load_operations, &label, &delta)) continue;
            *sample = (P5ThreadActivityRawSample){.schema_version = P5_THREAD_ACTIVITY_SCHEMA_VERSION, .run_index = run,
                .sample_index = count, .worker_index = activities[i].worker_index, .controlled_profile = profile, .intensity_percent = intensity,
                .sample_kind = P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL, .placement_mode = context->placement_mode,
                .worker_count = context->worker_count, .memory_mb = context->memory_mb, .load_operations_delta = delta,
                .interval_ms = activities[i].interval_ms, .metric_version = P5_THREAD_ACTIVITY_METRIC_VERSION, .valid = delta != 0 && activities[i].interval_ms != 0};
            (void)p5_thread_activity_rate(delta, activities[i].interval_ms, &sample->load_rate_ops_per_ms);
            snprintf(sample->calibration_id, sizeof(sample->calibration_id), "%s", id); snprintf(sample->metric_name, sizeof(sample->metric_name), "%s", P5_THREAD_ACTIVITY_METRIC_NAME);
            snprintf(sample->metric_unit, sizeof(sample->metric_unit), "%s", P5_THREAD_ACTIVITY_METRIC_UNIT); snprintf(sample->reason, sizeof(sample->reason), "%s", p5_c2a_collector_evidence_label_name(label));
        }
        if (waitpid(child, &status, WNOHANG) == child) { alive = false; *child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0; }
        usleep(10000);
    }
    if (alive) { kill(child, SIGTERM); (void)waitpid(child, &status, 0); }
    return count;
}

static int collect_placement(const Options *o, const benchmark_placement_topology_t *topology, worker_evidence_provider_t *provider,
                             P5ThreadActivityPlacementMode placement, const char *id)
{
    P5ThreadActivityContext c; P5ThreadActivityRawSample *samples; P5ThreadActivityCalibration calibration; P5C2ACollectorMatrixEntry matrix[3];
    size_t count = 0; char socket[108], reason[P5_THREAD_ACTIVITY_REASON_MAX];
    context(&c, o, placement); samples = calloc(MAX_SAMPLES, sizeof(*samples)); if (!samples) return -1;
    if (mkdir(o->root, 0700) != 0 && errno != EEXIST) { free(samples); return -1; }
    snprintf(socket, sizeof(socket), "/tmp/awavma-c2a-%ld-%c.sock", (long)getpid(), placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? 'l' : 'r');
    if (!worker_evidence_provider_start(provider, socket)) { free(samples); return -1; }
    (void)p5_c2a_collector_matrix_a(matrix, 3);
    for (unsigned p = 0; p < 3 && !interrupted; ++p) for (unsigned run = 0; run < o->runs && !interrupted; ++run) {
        bool child_ok;
        pid_t child = launch(o, topology, placement, matrix[p].intensity_percent, socket, run + p * o->runs);
        if (child <= 0) { worker_evidence_provider_stop(provider); free(samples); return -1; }
        count = collect_run(provider, child, samples, count, &c, id, run, matrix[p].profile, matrix[p].intensity_percent, &child_ok);
        if (!child_ok) { worker_evidence_provider_stop(provider); free(samples); return -1; }
    }
    worker_evidence_provider_stop(provider);
    if (interrupted || p5_thread_activity_calibration_build(&c, id, samples, count, &calibration, reason, sizeof(reason)) != 0 ||
        p5_thread_activity_calibration_write_artifacts(o->root, &c, id, samples, count, &calibration, reason, sizeof(reason)) != 0) {
        fprintf(stderr, "status=FAILED placement=%s reason=%s samples=%zu\n", placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "local" : "remote", reason, count); free(samples); return -1;
    }
    {
        char diagnostics[512]; FILE *file;
        snprintf(diagnostics, sizeof(diagnostics), "%s/c2a3_diagnostics.csv", o->root);
        file = fopen(diagnostics, "a");
        if (file != NULL) {
            if (ftell(file) == 0) fputs("schema_version,calibration_id,placement,start_node,end_node,numa_balancing,worker_evidence,diagnostic\n", file);
            fprintf(file, "1,%s,%s,%d,%d,disabled,authenticated,child placement evidence captured per run\n", id,
                    placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "LOCAL" : "REMOTE", topology->local_node,
                    placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? topology->local_node : topology->remote_node);
            fclose(file);
        }
    }
    printf("status=COLLECTED placement=%s calibration_id=%s samples=%zu calibration_status=%s\n", placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "local" : "remote", id, count, p5_thread_activity_calibration_status_name(calibration.status));
    free(samples); return 0;
}

int main(int argc, char **argv)
{
    Options options; benchmark_placement_topology_t topology = {0}; worker_evidence_provider_t *provider = NULL; int result = P5_C2A_COLLECTOR_ENV_LIMITED;
    if (!parse_options(argc, argv, &options)) return usage(argv[0]);
    if (benchmark_placement_discover(&topology) != 0) { printf("status=ENV_LIMITED reason=two_permitted_numa_nodes_required matrix=%s\n", P5_C2A_COLLECTOR_MATRIX_A_VERSION); return P5_C2A_COLLECTOR_ENV_LIMITED; }
    printf("status=PLANNED matrix=%s local_node=%d remote_node=%d distance=%d\n", P5_C2A_COLLECTOR_MATRIX_A_VERSION, topology.local_node, topology.remote_node, topology.numa_distance);
    if (!options.execute) return 0;
    if (!disable_numa_balancing()) { fprintf(stderr, "status=ENV_LIMITED reason=numa_balancing_control_unavailable\n"); return P5_C2A_COLLECTOR_ENV_LIMITED; }
    atexit(restore_numa_balancing); signal(SIGHUP, on_signal); signal(SIGINT, on_signal); signal(SIGQUIT, on_signal); signal(SIGTERM, on_signal);
    provider = worker_evidence_provider_create();
    if (provider != NULL && collect_placement(&options, &topology, provider, P5_THREAD_ACTIVITY_PLACEMENT_LOCAL, options.local_id) == 0 &&
        collect_placement(&options, &topology, provider, P5_THREAD_ACTIVITY_PLACEMENT_REMOTE, options.remote_id) == 0 && !interrupted) result = 0;
    worker_evidence_provider_destroy(provider); restore_numa_balancing();
    return result;
}
