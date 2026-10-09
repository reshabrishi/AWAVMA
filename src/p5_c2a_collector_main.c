#include "benchmark_placement.h"
#include "p5_c2a_collector.h"
#include "p5_thread_activity_calibration_io.h"
#include "page_candidate_provider.h"
#include "worker_evidence_provider.h"

#include <errno.h>
#include <signal.h>
#include <sched.h>
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
#define C2A_WARMUP_RUNS 2U
#define C2A_MEASURED_RUNS 7U
#define C2A_DISCARD_MS 2000U

typedef struct {
    bool dry_run, execute;
    const char *benchmark, *root, *local_id, *remote_id;
    unsigned workers, memory_mb, warmup_runs, measured_runs, duration_ms, discard_ms;
    uint64_t seed;
} Options;

typedef struct {
    const Options *options;
    const benchmark_placement_topology_t *topology;
    const char *calibration_id;
    P5ThreadActivityPlacementMode placement;
    P5ThreadActivityProfile profile;
    unsigned intensity_percent, run_index, run_kind_warmup, seed;
    const char *cpus;
    bool authoritative;
    int benchmark_exit_code;
    bool worker_evidence_ok, worker_seen[2], valid;
    uint64_t registration_generation;
    pid_t worker_tid[2];
    int worker_cpu[2], worker_package[2], worker_core[2];
    char worker_siblings[2][64];
    char start_status[32], end_status[32], restore_status[32], reason[64];
    benchmark_placement_evidence_t start_placement, end_placement;
} RunDiagnostic;

static char saved_numa_balancing[32];
static bool numa_balancing_changed;
static volatile sig_atomic_t interrupted;

static const char *diagnostic_header =
    "schema_version,calibration_id,placement_mode,controlled_profile,intensity_percent,run_kind,run_index,workers,memory_mb,duration_ms,startup_discard_ms,pattern,seed,local_node,requested_memory_node,numa_distance,numa_balancing_original,numa_balancing_during_run,numa_balancing_restore_status,worker0_tid,worker0_cpu,worker0_package,worker0_core,worker0_siblings,worker1_tid,worker1_cpu,worker1_package,worker1_core,worker1_siblings,smt_distinct,affinity_verified,registration_ok,registration_generation,start_total_pages,start_queryable_pages,start_expected_pages,start_local_pages,start_remote_pages,start_other_pages,start_unknown_pages,start_expected_ratio,start_status,end_total_pages,end_queryable_pages,end_expected_pages,end_local_pages,end_remote_pages,end_other_pages,end_unknown_pages,end_expected_ratio,end_status,worker_evidence_ok,worker0_evidence_seen,worker1_evidence_seen,benchmark_exit_code,valid,reason,authoritative\n";

static int read_int_file(const char *path)
{
    FILE *file = fopen(path, "r"); int value = -1;
    if (file != NULL) { if (fscanf(file, "%d", &value) != 1) value = -1; fclose(file); }
    return value;
}

static void cpu_metadata(int cpu, int *package_id, int *core_id, char *siblings, size_t siblings_size)
{
    char path[160]; FILE *file;
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu); *package_id = read_int_file(path);
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu); *core_id = read_int_file(path);
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    file = fopen(path, "r");
    if (file != NULL && fgets(siblings, siblings_size, file) != NULL) siblings[strcspn(siblings, "\n")] = '\0';
    else if (siblings_size != 0) siblings[0] = '\0';
    if (file != NULL) fclose(file);
}

static void diagnostic_init(RunDiagnostic *d, const Options *o, const benchmark_placement_topology_t *topology,
                            const char *id, P5ThreadActivityPlacementMode placement, P5ThreadActivityProfile profile,
                            unsigned intensity, unsigned run_index, bool warmup, const char *cpus, bool authoritative)
{
    memset(d, 0, sizeof(*d)); d->options = o; d->topology = topology; d->calibration_id = id; d->placement = placement;
    d->profile = profile; d->intensity_percent = intensity; d->run_index = run_index; d->run_kind_warmup = warmup ? 1U : 0U;
    d->seed = (unsigned)(o->seed + run_index); d->cpus = cpus; d->authoritative = authoritative; d->benchmark_exit_code = -1;
    snprintf(d->start_status, sizeof(d->start_status), "UNKNOWN"); snprintf(d->end_status, sizeof(d->end_status), "UNKNOWN");
    snprintf(d->restore_status, sizeof(d->restore_status), "PENDING"); snprintf(d->reason, sizeof(d->reason), "OK");
    for (int i = 0; i < 2; ++i) { d->worker_tid[i] = -1; d->worker_cpu[i] = -1; d->worker_package[i] = -1; d->worker_core[i] = -1; }
}

static bool read_placement_csv(const char *path, benchmark_placement_evidence_t *e)
{
    FILE *file = fopen(path, "r"); char header[1024], row[1024];
    if (file == NULL) return false;
    if (fgets(header, sizeof(header), file) == NULL || fgets(row, sizeof(row), file) == NULL) { fclose(file); return false; }
    fclose(file);
    char mode[32], restored[16]; double ratio;
    return sscanf(row, "%*u,%31[^,],%d,%d,%d,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%lf,%d,%31[^,],%159[^,],%15[^\n]",
                  mode, &e->local_node, &e->requested_memory_node, &e->numa_distance, &e->total_pages,
                  &e->queryable_pages, &e->expected_node_pages, &e->local_pages, &e->remote_pages,
                  &e->other_pages, &e->unknown_pages, &ratio, &e->observed_dominant_node,
                  e->verification_status, e->verification_reason, restored) >= 15;
}

static int write_diagnostic_row(const Options *o, const RunDiagnostic *d)
{
    char path[512]; FILE *file; bool header;
    snprintf(path, sizeof(path), "%s/calibration_runs.csv", o->root);
    file = fopen(path, "a+"); if (file == NULL) return -1;
    header = fseek(file, 0, SEEK_END) != 0 || ftell(file) == 0;
    if (header) fputs(diagnostic_header, file);
    double start_ratio = d->start_placement.total_pages == 0 ? 0.0 : (double)d->start_placement.expected_node_pages / d->start_placement.total_pages;
    double end_ratio = d->end_placement.total_pages == 0 ? 0.0 : (double)d->end_placement.expected_node_pages / d->end_placement.total_pages;
    fprintf(file, "1,%s,%s,%s,%u,%s,%u,%u,%u,%u,%u,random,%u,%d,%d,%d,%s,0,%s,%ld,%d,%d,%d,%s,%ld,%d,%d,%d,%s,%u,%u,%u,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.9f,%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.9f,%s,%u,%u,%u,%d,%u,%s,%u\n",
            d->calibration_id, d->placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "LOCAL" : "REMOTE",
            p5_thread_activity_profile_name(d->profile), d->intensity_percent, d->run_kind_warmup ? "WARMUP" : "MEASURED",
            d->run_index, o->workers, o->memory_mb, o->duration_ms, o->discard_ms, d->seed, d->topology->local_node,
            d->placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? d->topology->local_node : d->topology->remote_node, d->topology->numa_distance,
            saved_numa_balancing[0] ? saved_numa_balancing : "UNKNOWN", d->restore_status,
            (long)d->worker_tid[0], d->worker_cpu[0], d->worker_package[0], d->worker_core[0], d->worker_siblings[0],
            (long)d->worker_tid[1], d->worker_cpu[1], d->worker_package[1], d->worker_core[1], d->worker_siblings[1],
            d->worker_package[0] != d->worker_package[1] || d->worker_core[0] != d->worker_core[1],
            d->worker_cpu[0] >= 0 && d->worker_cpu[1] >= 0, d->registration_generation != 0,
            (unsigned long long)d->registration_generation,
            d->start_placement.total_pages, d->start_placement.queryable_pages, d->start_placement.expected_node_pages,
            d->start_placement.local_pages, d->start_placement.remote_pages, d->start_placement.other_pages, d->start_placement.unknown_pages,
            start_ratio, d->start_status,
            d->end_placement.total_pages, d->end_placement.queryable_pages, d->end_placement.expected_node_pages,
            d->end_placement.local_pages, d->end_placement.remote_pages, d->end_placement.other_pages, d->end_placement.unknown_pages,
            end_ratio, d->end_status, d->worker_evidence_ok, d->worker_seen[0], d->worker_seen[1], d->benchmark_exit_code,
            d->valid, d->reason, d->authoritative);
    fflush(file); fsync(fileno(file)); return fclose(file) == 0 ? 0 : -1;
}

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
    fprintf(stderr, "usage: %s [--matrix-a-v1] [--dry-run|--execute] [--benchmark PATH] [--output-root PATH]\\n"
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
        .remote_id = "c2a3-remote", .workers = 2, .memory_mb = 256, .warmup_runs = C2A_WARMUP_RUNS,
        .measured_runs = C2A_MEASURED_RUNS, .duration_ms = 20000, .discard_ms = C2A_DISCARD_MS, .seed = 12345};
    for (int i = 1; i < argc; ++i) {
        uint64_t value;
        if (!strcmp(argv[i], "--matrix-a-v1")) continue;
        else if (!strcmp(argv[i], "--dry-run")) options->dry_run = true;
        else if (!strcmp(argv[i], "--execute")) options->execute = true;
        else if ((!strcmp(argv[i], "--benchmark") || !strcmp(argv[i], "--benchmark-path")) && ++i < argc) options->benchmark = argv[i];
        else if (!strcmp(argv[i], "--output-root") && ++i < argc) options->root = argv[i];
        else if ((!strcmp(argv[i], "--local-id") || !strcmp(argv[i], "--local-calibration-id")) && ++i < argc) options->local_id = argv[i];
        else if ((!strcmp(argv[i], "--remote-id") || !strcmp(argv[i], "--remote-calibration-id")) && ++i < argc) options->remote_id = argv[i];
        else if ((!strcmp(argv[i], "--workers") || !strcmp(argv[i], "--worker-count")) && argument(&i, argc, argv, &value) && value <= 256) options->workers = (unsigned)value;
        else if (!strcmp(argv[i], "--memory-mb") && argument(&i, argc, argv, &value) && value <= UINT32_MAX) options->memory_mb = (unsigned)value;
        else if ((!strcmp(argv[i], "--runs") || !strcmp(argv[i], "--measured-runs")) && argument(&i, argc, argv, &value) && value <= 1000) options->measured_runs = (unsigned)value;
        else if (!strcmp(argv[i], "--warmup-runs") && argument(&i, argc, argv, &value) && value <= 1000) options->warmup_runs = (unsigned)value;
        else if (!strcmp(argv[i], "--discard-ms") && argument(&i, argc, argv, &value) && value <= 3600000) options->discard_ms = (unsigned)value;
        else if (!strcmp(argv[i], "--duration-ms") && argument(&i, argc, argv, &value) && value <= 3600000) options->duration_ms = (unsigned)value;
        else if (!strcmp(argv[i], "--seed") && argument(&i, argc, argv, &value)) options->seed = value;
        else return false;
    }
    return !(options->dry_run && options->execute) && p5_thread_activity_calibration_id_valid(options->local_id) &&
           p5_thread_activity_calibration_id_valid(options->remote_id) && strcmp(options->local_id, options->remote_id);
}

static bool numa_value_is(const char *expected)
{
    char value[32] = {0}; FILE *file = fopen(NUMA_BALANCING, "r");
    if (file == NULL) return false;
    bool match = fgets(value, sizeof(value), file) != NULL && !strcmp(value, expected);
    fclose(file); return match;
}

static bool disable_numa_balancing(void)
{
    FILE *file = fopen(NUMA_BALANCING, "r");
    if (file == NULL || fgets(saved_numa_balancing, sizeof(saved_numa_balancing), file) == NULL) { if (file) fclose(file); return false; }
    fclose(file);
    file = fopen(NUMA_BALANCING, "w");
    if (file == NULL) return false;
    numa_balancing_changed = true;
    if (fputs("0\n", file) == EOF || fclose(file) != 0 || !numa_value_is("0\n")) { restore_numa_balancing(); return false; }
    return true;
}

static void context(P5ThreadActivityContext *context, const Options *options,
                    const benchmark_placement_topology_t *topology, P5ThreadActivityPlacementMode placement)
{
    struct utsname name;
    memset(context, 0, sizeof(*context)); (void)uname(&name);
    context->schema_version = P5_THREAD_ACTIVITY_SCHEMA_VERSION;
    snprintf(context->architecture, sizeof(context->architecture), "%s", name.machine[0] ? name.machine : "unknown");
    {
        FILE *cpuinfo = fopen("/proc/cpuinfo", "r"); char line[512];
        snprintf(context->cpu_vendor, sizeof(context->cpu_vendor), "unknown");
        snprintf(context->cpu_model_name, sizeof(context->cpu_model_name), "unknown");
        while (cpuinfo != NULL && fgets(line, sizeof(line), cpuinfo) != NULL) {
            char *value = strchr(line, ':');
            if (value == NULL) continue;
            value += 2; value[strcspn(value, "\n")] = '\0';
            if (!strncmp(line, "vendor_id", 9)) snprintf(context->cpu_vendor, sizeof(context->cpu_vendor), "%s", value);
            else if (!strncmp(line, "model name", 10)) snprintf(context->cpu_model_name, sizeof(context->cpu_model_name), "%s", value);
        }
        if (cpuinfo != NULL) fclose(cpuinfo);
    }
    context->online_cpu_count = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN); context->numa_node_count = topology->permitted_node_count;
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
                    unsigned intensity, const char *socket, const char *page_socket, const char *cpus, unsigned run, const char *evidence)
{
    char workers[16], memory[16], duration[16], seed[32], thread_node[16], intensity_text[16]; pid_t child = fork();
    if (child != 0) return child;
    snprintf(workers, sizeof(workers), "%u", o->workers); snprintf(memory, sizeof(memory), "%u", o->memory_mb);
    snprintf(duration, sizeof(duration), "%.3f", (double)o->duration_ms / 1000.0); snprintf(seed, sizeof(seed), "%llu", (unsigned long long)(o->seed + run));
    snprintf(thread_node, sizeof(thread_node), "%d", t->local_node); snprintf(intensity_text, sizeof(intensity_text), "%u", intensity);
    char *const argv[] = {(char *)o->benchmark, "--threads", workers, "--memory", memory, "--duration", duration,
        "--pattern", "random", "--seed", seed, "--thread-node", thread_node,
        "--placement-mode", placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "local" : "remote",
        "--intensity-percent", intensity_text, "--worker-evidence-socket", (char *)socket,
        "--page-registration-socket", (char *)page_socket, "--page-registration-required", "--worker-cpus", (char *)cpus,
        "--placement-evidence", (char *)evidence, NULL};
    execv(o->benchmark, argv); _exit(127);
}

static size_t collect_run(worker_evidence_provider_t *provider, page_candidate_provider_t *pages, pid_t child, P5ThreadActivityRawSample *samples, size_t count,
                          const P5ThreadActivityContext *context, const char *id, unsigned run, P5ThreadActivityProfile profile, unsigned intensity,
                          bool warmup, unsigned discard_ms, bool *child_ok, bool *evidence_ok, RunDiagnostic *diagnostic)
{
    uint64_t generations[WORKER_EVIDENCE_MAX_WORKERS] = {0}, sample_indexes[WORKER_EVIDENCE_MAX_WORKERS] = {0}, elapsed_ms[WORKER_EVIDENCE_MAX_WORKERS] = {0};
    uint64_t registration_generation = 0; bool worker_seen[WORKER_EVIDENCE_MAX_WORKERS] = {0}; unsigned workers_seen = 0; int status; bool alive = true;
    *child_ok = false; *evidence_ok = false;
    while (alive && !interrupted) {
        worker_evidence_activity_t activities[WORKER_EVIDENCE_MAX_WORKERS]; size_t n;
        (void)worker_evidence_provider_poll(provider); (void)page_candidate_provider_poll(pages); n = worker_evidence_provider_snapshot(provider, child, activities, WORKER_EVIDENCE_MAX_WORKERS);
        for (size_t i = 0; i < n && count < MAX_SAMPLES; ++i) if (activities[i].worker_index < WORKER_EVIDENCE_MAX_WORKERS && activities[i].evidence_generation > generations[activities[i].worker_index]) {
            P5ThreadActivityRawSample *sample = &samples[count++]; P5C2AEvidenceLabel label; uint64_t delta; bool gap = generations[activities[i].worker_index] != 0 && activities[i].evidence_generation != generations[activities[i].worker_index] + 1;
            if (activities[i].registration_generation == 0 || (registration_generation != 0 && activities[i].registration_generation != registration_generation)) continue;
            if (registration_generation == 0) registration_generation = activities[i].registration_generation;
            generations[activities[i].worker_index] = activities[i].evidence_generation;
            elapsed_ms[activities[i].worker_index] += activities[i].interval_ms;
            if (!p5_c2a_collector_label_delta(activities[i].evidence_generation == 1 ? 0 : activities[i].registered_memory_load_operations - activities[i].load_operations_delta,
                                              activities[i].registered_memory_load_operations, &label, &delta)) continue;
            *sample = (P5ThreadActivityRawSample){.schema_version = P5_THREAD_ACTIVITY_SCHEMA_VERSION, .run_index = run,
                .sample_index = ++sample_indexes[activities[i].worker_index], .worker_index = activities[i].worker_index, .controlled_profile = profile, .intensity_percent = intensity,
                .sample_kind = warmup || elapsed_ms[activities[i].worker_index] <= discard_ms ? P5_THREAD_ACTIVITY_SAMPLE_WARMUP_INTERVAL : P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL, .placement_mode = context->placement_mode,
                .worker_count = context->worker_count, .memory_mb = context->memory_mb, .load_operations_delta = delta,
                .interval_ms = activities[i].interval_ms, .metric_version = P5_THREAD_ACTIVITY_METRIC_VERSION, .valid = !warmup && !gap && elapsed_ms[activities[i].worker_index] > discard_ms && activities[i].interval_ms != 0 && delta != 0};
            if (activities[i].worker_index < 2) {
                unsigned wi = activities[i].worker_index;
                diagnostic->worker_tid[wi] = activities[i].tid;
                diagnostic->registration_generation = activities[i].registration_generation;
                if (sample->valid && !worker_seen[wi]) { worker_seen[wi] = true; diagnostic->worker_seen[wi] = true; workers_seen++; }
            }
            (void)p5_thread_activity_rate(delta, activities[i].interval_ms, &sample->load_rate_ops_per_ms);
            snprintf(sample->calibration_id, sizeof(sample->calibration_id), "%s", id); snprintf(sample->metric_name, sizeof(sample->metric_name), "%s", P5_THREAD_ACTIVITY_METRIC_NAME);
            snprintf(sample->metric_unit, sizeof(sample->metric_unit), "%s", P5_THREAD_ACTIVITY_METRIC_UNIT); snprintf(sample->reason, sizeof(sample->reason), "%s_%s%s", warmup ? "WARMUP" : "RUN", p5_c2a_collector_evidence_label_name(label), gap ? "_GAP" : "");
        }
        if (waitpid(child, &status, WNOHANG) == child) { alive = false; diagnostic->benchmark_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1; *child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0; }
        usleep(10000);
    }
    if (alive) { kill(child, SIGTERM); (void)waitpid(child, &status, 0); }
    *evidence_ok = registration_generation != 0 && workers_seen == context->worker_count;
    diagnostic->worker_evidence_ok = *evidence_ok;
    return count;
}

static bool selected_cpus(int node, unsigned workers, char *output, size_t output_size)
{
    cpu_set_t allowed; int selected[CPU_SETSIZE]; size_t count = 0, used = 0;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
    for (int cpu = 0; cpu < CPU_SETSIZE && count < workers; ++cpu) {
        char package_path[128], core_path[128], node_path[128]; FILE *package, *core;
        int package_id, core_id; bool duplicate = false;
        if (!CPU_ISSET(cpu, &allowed)) continue;
        snprintf(node_path, sizeof(node_path), "/sys/devices/system/cpu/cpu%d/node%d", cpu, node);
        if (access(node_path, F_OK) != 0) continue;
        snprintf(package_path, sizeof(package_path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
        snprintf(core_path, sizeof(core_path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        package = fopen(package_path, "r"); core = fopen(core_path, "r");
        if (package == NULL || core == NULL || fscanf(package, "%d", &package_id) != 1 || fscanf(core, "%d", &core_id) != 1) { if (package) fclose(package); if (core) fclose(core); continue; }
        fclose(package); fclose(core);
        for (size_t prior = 0; prior < count; ++prior) {
            char pp[128], cp[128]; int prior_package, prior_core; FILE *pf, *cf;
            snprintf(pp, sizeof(pp), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", selected[prior]);
            snprintf(cp, sizeof(cp), "/sys/devices/system/cpu/cpu%d/topology/core_id", selected[prior]);
            pf = fopen(pp, "r"); cf = fopen(cp, "r");
            if (pf && cf && fscanf(pf, "%d", &prior_package) == 1 && fscanf(cf, "%d", &prior_core) == 1 && prior_package == package_id && prior_core == core_id) duplicate = true;
            if (pf) fclose(pf);
            if (cf) fclose(cf);
        }
        if (!duplicate) selected[count++] = cpu;
    }
    if (count != workers) return false;
    output[0] = '\0';
    for (size_t index = 0; index < count; ++index) {
        int written = snprintf(output + used, output_size - used, "%s%d", index ? "," : "", selected[index]);
        if (written < 0 || (size_t)written >= output_size - used) return false;
        used += (size_t)written;
    }
    return true;
}

static int collect_placement(const Options *o, const benchmark_placement_topology_t *topology, worker_evidence_provider_t *provider,
                             page_candidate_provider_t *pages, const char *page_socket, P5ThreadActivityPlacementMode placement, const char *id, bool authoritative)
{
    P5ThreadActivityContext c; P5ThreadActivityRawSample *samples; P5ThreadActivityCalibration calibration; P5C2ACollectorMatrixEntry matrix[3];
    size_t count = 0; char socket[108], cpus[256], reason[P5_THREAD_ACTIVITY_REASON_MAX] = {0}; unsigned global_run = 0;
    context(&c, o, topology, placement); samples = calloc(MAX_SAMPLES, sizeof(*samples)); if (!samples) return -1;
    if (mkdir(o->root, 0700) != 0 && errno != EEXIST) { free(samples); return -1; }
    snprintf(socket, sizeof(socket), "/tmp/awavma-c2a-%ld-%c.sock", (long)getpid(), placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? 'l' : 'r');
    if (!selected_cpus(topology->local_node, o->workers, cpus, sizeof(cpus))) { free(samples); return -1; }
    if (!worker_evidence_provider_start(provider, socket)) { free(samples); return -1; }
    (void)p5_c2a_collector_matrix_a(matrix, 3);
    for (unsigned p = 0; p < 3 && !interrupted; ++p) for (unsigned run = 0; run < o->warmup_runs + o->measured_runs && !interrupted; ++run) {
        bool child_ok, evidence_ok; char placement_path[512]; RunDiagnostic diagnostic;
        bool warmup = run < o->warmup_runs;
        if (!numa_value_is("0\n") && !disable_numa_balancing()) { worker_evidence_provider_stop(provider); free(samples); return -1; }
        unsigned run_index = global_run++;
        snprintf(placement_path, sizeof(placement_path), "%s/c2a3-%c-%u-placement.csv", o->root, placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? 'l' : 'r', run_index);
        diagnostic_init(&diagnostic, o, topology, id, placement, matrix[p].profile, matrix[p].intensity_percent, run_index, warmup, cpus, authoritative);
        for (unsigned wi = 0; wi < o->workers && wi < 2; ++wi) {
            int cpu = -1; const char *cursor = cpus;
            for (unsigned ci = 0; ci <= wi && cursor != NULL; ++ci) { cpu = atoi(cursor); cursor = strchr(cursor, ','); if (cursor != NULL) cursor++; }
            diagnostic.worker_cpu[wi] = cpu; cpu_metadata(cpu, &diagnostic.worker_package[wi], &diagnostic.worker_core[wi], diagnostic.worker_siblings[wi], sizeof(diagnostic.worker_siblings[wi]));
        }
        pid_t child = launch(o, topology, placement, matrix[p].intensity_percent, socket, page_socket, cpus, run_index, placement_path);
        if (child <= 0) { worker_evidence_provider_stop(provider); free(samples); return -1; }
        count = collect_run(provider, pages, child, samples, count, &c, id, run_index, matrix[p].profile, matrix[p].intensity_percent, warmup, o->discard_ms, &child_ok, &evidence_ok, &diagnostic);
        char start_path[sizeof(placement_path) + 8];
        snprintf(start_path, sizeof(start_path), "%s.start", placement_path);
        if (read_placement_csv(start_path, &diagnostic.start_placement)) {
            snprintf(diagnostic.start_status, sizeof(diagnostic.start_status), "%s", diagnostic.start_placement.verification_status);
        }
        if (read_placement_csv(placement_path, &diagnostic.end_placement)) {
            snprintf(diagnostic.end_status, sizeof(diagnostic.end_status), "%s", diagnostic.end_placement.verification_status);
        }
        restore_numa_balancing();
        bool restored = numa_value_is(saved_numa_balancing);
        snprintf(diagnostic.restore_status, sizeof(diagnostic.restore_status), "%s", restored ? "RESTORED" : "RESTORE_FAILED");
        diagnostic.valid = child_ok && (warmup || evidence_ok);
        snprintf(diagnostic.reason, sizeof(diagnostic.reason), "%s", !child_ok ? "BENCHMARK_EXIT_NONZERO" : (!warmup && !evidence_ok) ? "WORKER_EVIDENCE_INCOMPLETE" : "OK");
        if (!restored) { diagnostic.valid = false; snprintf(diagnostic.reason, sizeof(diagnostic.reason), "NUMA_BALANCING_RESTORE_FAILED"); }
        (void)write_diagnostic_row(o, &diagnostic);
        if (!diagnostic.valid) { worker_evidence_provider_stop(provider); free(samples); return -1; }
    }
    worker_evidence_provider_stop(provider);
    if (interrupted || (authoritative && (p5_thread_activity_calibration_build(&c, id, samples, count, &calibration, reason, sizeof(reason)) != 0 ||
        p5_thread_activity_calibration_write_artifacts(o->root, &c, id, samples, count, &calibration, reason, sizeof(reason)) != 0))) {
        fprintf(stderr, "status=FAILED placement=%s reason=%s samples=%zu\n", placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "local" : "remote", reason, count); free(samples); return -1;
    }
    printf("status=COLLECTED placement=%s calibration_id=%s samples=%zu authority=%s calibration_status=%s\n", placement == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "local" : "remote", id, count, authoritative ? "AUTHORITATIVE" : "SMOKE", authoritative ? p5_thread_activity_calibration_status_name(calibration.status) : "NOT_WRITTEN");
    free(samples); return 0;
}

int main(int argc, char **argv)
{
    Options options; benchmark_placement_topology_t topology = {0}; worker_evidence_provider_t *provider = NULL; page_candidate_provider_t *pages = NULL; char page_socket[108]; bool authoritative; int result = P5_C2A_COLLECTOR_ENV_LIMITED;
    if (!parse_options(argc, argv, &options)) return usage(argv[0]);
    authoritative = options.workers == 2 && options.memory_mb == 256 && options.duration_ms == 20000 && options.warmup_runs == C2A_WARMUP_RUNS && options.measured_runs == C2A_MEASURED_RUNS && options.discard_ms == C2A_DISCARD_MS;
    printf("status=PLANNED matrix=%s configurations=%u authority=%s workers=%u memory_mb=%u duration_ms=%u warmup_runs=%u measured_runs=%u discard_ms=%u\n", P5_C2A_COLLECTOR_MATRIX_A_VERSION, 3U * 2U * (options.warmup_runs + options.measured_runs), authoritative ? "AUTHORITATIVE" : "SMOKE", options.workers, options.memory_mb, options.duration_ms, options.warmup_runs, options.measured_runs, options.discard_ms);
    if (benchmark_placement_discover(&topology) != 0) { printf("status=ENV_LIMITED reason=two_permitted_numa_nodes_required matrix=%s\n", P5_C2A_COLLECTOR_MATRIX_A_VERSION); return P5_C2A_COLLECTOR_ENV_LIMITED; }
    printf("status=TOPOLOGY local_node=%d remote_node=%d distance=%d\n", topology.local_node, topology.remote_node, topology.numa_distance);
    if (!options.execute) return 0;
    if (!disable_numa_balancing()) { fprintf(stderr, "status=ENV_LIMITED reason=numa_balancing_control_unavailable\n"); return P5_C2A_COLLECTOR_ENV_LIMITED; }
    atexit(restore_numa_balancing); signal(SIGHUP, on_signal); signal(SIGINT, on_signal); signal(SIGQUIT, on_signal); signal(SIGTERM, on_signal);
    provider = worker_evidence_provider_create();
    pages = page_candidate_provider_create();
    snprintf(page_socket, sizeof(page_socket), "/tmp/awavma-c2a-pages-%ld.sock", (long)getpid());
    if (provider != NULL && pages != NULL && page_candidate_provider_start(pages, page_socket, 5000) &&
        collect_placement(&options, &topology, provider, pages, page_socket, P5_THREAD_ACTIVITY_PLACEMENT_LOCAL, options.local_id, authoritative) == 0 &&
        collect_placement(&options, &topology, provider, pages, page_socket, P5_THREAD_ACTIVITY_PLACEMENT_REMOTE, options.remote_id, authoritative) == 0 && !interrupted) result = 0;
    page_candidate_provider_stop(pages);
    page_candidate_provider_destroy(pages);
    worker_evidence_provider_destroy(provider); restore_numa_balancing();
    if (!numa_value_is(saved_numa_balancing)) { fprintf(stderr, "status=FAILED reason=numa_balancing_restore_readback_failed\n"); return P5_C2A_COLLECTOR_ENV_LIMITED; }
    return result;
}
