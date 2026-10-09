#include "benchmark_placement.h"
#include "p5_c2a_collector.h"
#include "p5_c2b_locality.h"
#include "page_candidate_provider.h"
#include "worker_evidence_provider.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define NUMA_BALANCING "/proc/sys/kernel/numa_balancing"
#define ENV_LIMITED 3
#define FULL_WARMUPS 2U
#define FULL_MEASURED 7U
#define DEFAULT_DURATION_MS 20000U
#define DEFAULT_MEMORY_MB 256U
#define DEFAULT_DISCARD_MS 2000U
#define SMOKE_MEMORY_MB 64U
#define SMOKE_DURATION_MS 5000U
#define SMOKE_DISCARD_MS 1000U
#define BASE_SEED 12345U
#define MAX_WINDOWS 128U

typedef struct {
    const char *benchmark, *root, *id;
    bool execute, dry_run, smoke, protocol_override, authoritative;
    unsigned warmups, measured, memory_mb, duration_ms, discard_ms;
} Options;

typedef struct {
    char original[32];
    bool active;
} NumaTransaction;

typedef struct {
    uint64_t operations, milliseconds, intervals, window_operations, window_ms, partial_intervals;
    double windows[MAX_WINDOWS];
    size_t window_count;
    bool evidence_seen;
    pid_t tid;
} WorkerRun;

typedef struct {
    FILE *raw;
    WorkerRun workers[2];
    uint64_t registration_generation, raw_count;
    int exit_code;
    bool evidence_ok, gap;
} RunOutput;

typedef struct {
    int cpu, package, core;
    char siblings[64];
} CpuMetadata;

static NumaTransaction active_numa;
static volatile sig_atomic_t stopped;

static bool read_text(const char *path, char *value, size_t size)
{
    FILE *file = fopen(path, "r");
    if (file == NULL || fgets(value, size, file) == NULL) { if (file) fclose(file); return false; }
    fclose(file); return true;
}

static bool numa_value_is(const char *expected)
{
    char value[32]; return read_text(NUMA_BALANCING, value, sizeof(value)) && strcmp(value, expected) == 0;
}

static bool numa_restore(NumaTransaction *transaction)
{
    FILE *file;
    if (transaction == NULL || !transaction->active) return false;
    file = fopen(NUMA_BALANCING, "w");
    if (file == NULL) return false;
    if (fputs(transaction->original, file) == EOF) { fclose(file); return false; }
    if (fclose(file) != 0) return false;
    transaction->active = false;
    return numa_value_is(transaction->original);
}

static bool numa_begin(NumaTransaction *transaction)
{
    FILE *file;
    memset(transaction, 0, sizeof(*transaction));
    if (!read_text(NUMA_BALANCING, transaction->original, sizeof(transaction->original))) return false;
    transaction->active = true;
    file = fopen(NUMA_BALANCING, "w");
    if (file == NULL) { (void)numa_restore(transaction); return false; }
    if (fputs("0\n", file) == EOF) { fclose(file); (void)numa_restore(transaction); return false; }
    if (fclose(file) != 0 || !numa_value_is("0\n")) {
        (void)numa_restore(transaction); return false;
    }
    return true;
}

static void emergency_restore(void) { if (active_numa.active) (void)numa_restore(&active_numa); }
static void signal_handler(int signal_number) { (void)signal_number; stopped = 1; }

static bool parse_number(const char *text, unsigned *value)
{
    char *end; unsigned long parsed;
    errno = 0; parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || parsed == 0 || parsed > 3600000UL) return false;
    *value = (unsigned)parsed; return true;
}

static bool parse_options(int argc, char **argv, Options *options)
{
    *options = (Options){.benchmark = "./bin/benchmark", .root = "results", .warmups = FULL_WARMUPS,
        .measured = FULL_MEASURED, .memory_mb = DEFAULT_MEMORY_MB, .duration_ms = DEFAULT_DURATION_MS,
        .discard_ms = DEFAULT_DISCARD_MS};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--matrix-b-v1")) continue;
        if (!strcmp(argv[i], "--dry-run")) options->dry_run = true;
        else if (!strcmp(argv[i], "--execute")) options->execute = true;
        else if (!strcmp(argv[i], "--smoke")) options->smoke = options->protocol_override = true;
        else if (!strcmp(argv[i], "--benchmark") && ++i < argc) options->benchmark = argv[i];
        else if (!strcmp(argv[i], "--output-root") && ++i < argc) options->root = argv[i];
        else if (!strcmp(argv[i], "--id") && ++i < argc) options->id = argv[i];
        else if (!strcmp(argv[i], "--duration-ms") && ++i < argc && parse_number(argv[i], &options->duration_ms)) options->protocol_override = true;
        else return false;
    }
    if (options->smoke) {
        options->warmups = 1; options->measured = 1; options->memory_mb = SMOKE_MEMORY_MB;
        options->duration_ms = SMOKE_DURATION_MS; options->discard_ms = SMOKE_DISCARD_MS;
    }
    options->authoritative = !options->protocol_override && options->warmups == FULL_WARMUPS &&
        options->measured == FULL_MEASURED && options->memory_mb == DEFAULT_MEMORY_MB &&
        options->duration_ms == DEFAULT_DURATION_MS && options->discard_ms == DEFAULT_DISCARD_MS;
    return options->id != NULL && p5_c2b_identifier_valid(options->id) && !(options->dry_run && options->execute);
}

static bool select_cpus(int node, int selected[2], CpuMetadata metadata[2], char *text, size_t size)
{
    cpu_set_t allowed; P5C2ACollectorCpu entries[CPU_SETSIZE]; size_t count = 0;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &allowed)) {
        char path[160]; FILE *file; int package, core;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/node%d", cpu, node); if (access(path, F_OK) != 0) continue;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
        file = fopen(path, "r"); if (!file || fscanf(file, "%d", &package) != 1) { if (file) fclose(file); continue; } fclose(file);
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        file = fopen(path, "r"); if (!file || fscanf(file, "%d", &core) != 1) { if (file) fclose(file); continue; } fclose(file);
        entries[count++] = (P5C2ACollectorCpu){cpu, node, package, core, true};
    }
    if (p5_c2a_collector_select_physical_cores(entries, count, node, selected, 2) != 2) return false;
    for (unsigned i = 0; i < 2; ++i) {
        char path[160]; FILE *file;
        metadata[i] = (CpuMetadata){.cpu = selected[i], .package = -1, .core = -1};
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", selected[i]);
        file = fopen(path, "r"); if (file) { if (fscanf(file, "%d", &metadata[i].package) != 1) metadata[i].package = -1; fclose(file); }
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", selected[i]);
        file = fopen(path, "r"); if (file) { if (fscanf(file, "%d", &metadata[i].core) != 1) metadata[i].core = -1; fclose(file); }
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", selected[i]);
        if (!read_text(path, metadata[i].siblings, sizeof(metadata[i].siblings))) metadata[i].siblings[0] = '\0';
        metadata[i].siblings[strcspn(metadata[i].siblings, "\r\n")] = '\0';
    }
    return snprintf(text, size, "%d,%d", selected[0], selected[1]) > 0;
}

static bool read_placement(const char *path, benchmark_placement_evidence_t *evidence)
{
    FILE *file = fopen(path, "r"); char header[1024], row[1024];
    if (!file || !fgets(header, sizeof(header), file) || !fgets(row, sizeof(row), file)) { if (file) fclose(file); return false; }
    fclose(file);
    return p5_c2b_parse_placement_row(row, evidence);
}

static bool placement_valid(const benchmark_placement_evidence_t *evidence, const benchmark_placement_topology_t *topology, int requested)
{
    return evidence->local_node == topology->local_node && evidence->remote_node == topology->remote_node &&
        evidence->requested_memory_node == requested && evidence->numa_distance == topology->numa_distance &&
        evidence->total_pages != 0 && evidence->queryable_pages == evidence->total_pages &&
        evidence->expected_node_pages == evidence->total_pages && evidence->other_pages == 0 && evidence->unknown_pages == 0 &&
        evidence->memory_policy_restored &&
        !strcmp(evidence->verification_status, "PASS");
}

static pid_t launch(const Options *options, const benchmark_placement_topology_t *topology, const P5C2BCell *cell,
                    unsigned run_index, const char *cpus, const char *worker_socket, const char *page_socket, const char *placement_path)
{
    char memory[24], duration[24], seed[24], node[16]; pid_t child = fork();
    if (child != 0) return child;
    snprintf(duration, sizeof(duration), "%.3f", (double)options->duration_ms / 1000.0);
    snprintf(memory, sizeof(memory), "%u", options->memory_mb);
    snprintf(seed, sizeof(seed), "%u", BASE_SEED + run_index); snprintf(node, sizeof(node), "%d", topology->local_node);
    char *const arguments[] = {(char *)options->benchmark, "--threads", "2", "--memory", memory, "--duration", duration,
        "--pattern", (char *)cell->pattern, "--seed", seed, "--thread-node", node,
        "--placement-mode", !strcmp(cell->placement, "LOCAL") ? "local" : "remote", "--intensity-percent", "100",
        "--worker-cpus", (char *)cpus, "--worker-evidence-socket", (char *)worker_socket,
        "--page-registration-socket", (char *)page_socket, "--page-registration-required",
        "--placement-evidence", (char *)placement_path, NULL};
    execv(options->benchmark, arguments); _exit(127);
}

static bool collect(worker_evidence_provider_t *provider, page_candidate_provider_t *pages, pid_t child,
                    const P5C2BCell *cell, unsigned run_index, bool warmup, unsigned discard_ms,
                    RunOutput *output, const char *id)
{
    uint64_t generation[2] = {0}, elapsed[2] = {0}, interval_index[2] = {0}; int status = 0; bool alive = true;
    while (alive && !stopped) {
        worker_evidence_activity_t activities[WORKER_EVIDENCE_MAX_WORKERS];
        (void)worker_evidence_provider_poll(provider); (void)page_candidate_provider_poll(pages);
        size_t count = worker_evidence_provider_snapshot(provider, child, activities, WORKER_EVIDENCE_MAX_WORKERS);
        for (size_t i = 0; i < count; ++i) if (activities[i].worker_index < 2 && activities[i].evidence_generation > generation[activities[i].worker_index]) {
            unsigned worker = activities[i].worker_index; WorkerRun *result = &output->workers[worker]; double rate;
            bool gap = generation[worker] && activities[i].evidence_generation != generation[worker] + 1;
            if (!output->registration_generation) output->registration_generation = activities[i].registration_generation;
            if (!activities[i].registration_generation || activities[i].registration_generation != output->registration_generation) gap = true;
            generation[worker] = activities[i].evidence_generation; elapsed[worker] += activities[i].interval_ms;
            bool cadence = activities[i].interval_ms >= 50 && activities[i].interval_ms <= 250;
            bool measured = !warmup && elapsed[worker] > discard_ms && !gap && cadence &&
                p5_c2b_rate(activities[i].load_operations_delta, activities[i].interval_ms, &rate);
            if (!measured) rate = activities[i].interval_ms ? (double)activities[i].load_operations_delta / activities[i].interval_ms : 0.0;
            fprintf(output->raw, "%u,%s,%u,%s,%s,%s,%u,%llu,%llu,%llu,%.17g,%s,%s,%u,%s\n", P5_C2B_SCHEMA_VERSION,
                id, run_index, warmup ? "WARMUP" : "MEASURED", cell->pattern, cell->placement, worker,
                (unsigned long long)++interval_index[worker], (unsigned long long)activities[i].load_operations_delta,
                (unsigned long long)activities[i].interval_ms, rate, P5_C2B_METRIC, P5_C2B_METRIC_UNIT, measured,
                warmup || elapsed[worker] <= discard_ms ? "DISCARDED" : gap ? "EVIDENCE_GAP" : !cadence ? "CADENCE_INVALID" : "OK");
            output->raw_count++; result->evidence_seen = true; result->tid = activities[i].tid;
            if (gap || !cadence) { output->gap = true; result->partial_intervals = result->window_operations = result->window_ms = 0; }
            if (measured) {
                result->operations += activities[i].load_operations_delta; result->milliseconds += activities[i].interval_ms; result->intervals++;
                result->window_operations += activities[i].load_operations_delta; result->window_ms += activities[i].interval_ms;
                if (++result->partial_intervals == P5_C2B_WINDOW_INTERVALS) {
                    if (result->window_count < MAX_WINDOWS) result->windows[result->window_count++] = (double)result->window_operations / result->window_ms;
                    result->partial_intervals = result->window_operations = result->window_ms = 0;
                }
            }
        }
        if (waitpid(child, &status, WNOHANG) == child) alive = false; else usleep(10000);
    }
    if (alive) { kill(child, SIGTERM); (void)waitpid(child, &status, 0); }
    output->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    output->evidence_ok = generation[0] && generation[1] && output->registration_generation && !output->gap &&
        (warmup || (output->workers[0].window_count && output->workers[1].window_count));
    return !stopped && output->exit_code == 0 && output->evidence_ok;
}

static bool hardware_fingerprint(const benchmark_placement_topology_t *topology, char *output, size_t size)
{
    struct utsname name; FILE *file; char *text = NULL, line[512]; size_t used = 0, capacity = 0; uint32_t family, model;
    if (uname(&name) != 0 || (file = fopen("/proc/cpuinfo", "r")) == NULL) return false;
    while (fgets(line, sizeof(line), file)) { size_t length = strlen(line); if (used + length + 1 > capacity) { size_t next = capacity ? capacity * 2 : 4096; while (next < used + length + 1) next *= 2; char *grown = realloc(text, next); if (!grown) { free(text); fclose(file); return false; } text = grown; capacity = next; } memcpy(text + used, line, length); used += length; text[used] = '\0'; }
    fclose(file); bool parsed = p5_c2a_collector_parse_cpu_family_model(text, &family, &model); free(text);
    return parsed && snprintf(output, size, "arch=%s|family=%u|model=%u|cpus=%ld|nodes=%u|local=%d|remote=%d|distance=%d",
        name.machine, family, model, sysconf(_SC_NPROCESSORS_ONLN), topology->permitted_node_count,
        topology->local_node, topology->remote_node, topology->numa_distance) < (int)size;
}

static int write_manifest(const Options *options, const benchmark_placement_topology_t *topology, const char *directory,
                          double units[6][P5_C2B_EXPECTED_UNITS], const size_t unit_counts[6], uint64_t raw_count)
{
    char path[768], fingerprint[256], escaped[520]; FILE *file; P5C2BCell cells[6]; bool complete = true;
    if (!hardware_fingerprint(topology, fingerprint, sizeof(fingerprint)) || !p5_c2a_collector_csv_text(fingerprint, escaped, sizeof(escaped))) return -1;
    snprintf(path, sizeof(path), "%s/locality_manifest.csv", directory); file = fopen(path, "w"); if (!file) return -1;
    fprintf(file, "schema_version,format_version,validation_id,matrix_version,hardware_fingerprint,pattern,placement,local_node,remote_node,numa_distance,workers,memory_mb,duration_ms,startup_discard_ms,seed_base,benchmark_definition_version,pattern_version,intensity_percent,numa_balancing_state,warmup_runs,measured_runs,run_count,raw_interval_count,worker_run_count,minimum_worker_run_units,status,metric_name,metric_unit,minimum,maximum,mean,median,population_stddev,p10,p90,classification_authority,runtime_authority,expected_gain_authority,production_migration_authority,authoritative\n");
    (void)p5_c2b_matrix(cells, 6);
    for (unsigned cell = 0; cell < 6; ++cell) {
        P5C2BStatistics statistics = {0}; bool enough = unit_counts[cell] >= P5_C2B_MINIMUM_UNITS;
        complete = complete && (!options->authoritative || enough);
        if (!p5_c2b_statistics(units[cell], unit_counts[cell], &statistics)) { fclose(file); return -1; }
        fprintf(file, "%u,1,%s,%s,%s,%s,%s,%d,%d,%d,2,%u,%u,%u,%u,benchmark-locality-intensity-v1,locality-patterns-v1,100,disabled,%u,%u,%u,%llu,%zu,%u,%s,%s,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,UNAVAILABLE,DISABLED,DISABLED,DISABLED,%u\n",
            P5_C2B_SCHEMA_VERSION, options->id, P5_C2B_MATRIX_VERSION, escaped, cells[cell].pattern, cells[cell].placement,
            topology->local_node, topology->remote_node, topology->numa_distance, options->memory_mb, options->duration_ms, options->discard_ms, BASE_SEED, options->warmups, options->measured,
            6U * (options->warmups + options->measured), (unsigned long long)raw_count, unit_counts[cell], P5_C2B_MINIMUM_UNITS,
            options->authoritative ? (enough ? "DESCRIPTIVE_VALID" : "INSUFFICIENT_UNITS") : options->smoke ? "SMOKE_VALID" : "NON_AUTHORITATIVE_VALID",
            P5_C2B_METRIC, P5_C2B_METRIC_UNIT, statistics.minimum, statistics.maximum, statistics.mean, statistics.median,
            statistics.population_stddev, statistics.p10, statistics.p90, options->authoritative);
    }
    return fclose(file) == 0 && complete ? 0 : -1;
}

static int execute(const Options *options, const benchmark_placement_topology_t *topology, const char *directory)
{
    char path[768], worker_socket[108], page_socket[108], cpus[128]; int selected[2]; CpuMetadata cpu[2];
    FILE *raw = NULL, *summary = NULL, *runs = NULL; worker_evidence_provider_t *provider = NULL; page_candidate_provider_t *pages = NULL;
    double units[6][P5_C2B_EXPECTED_UNITS] = {{0}}; size_t unit_counts[6] = {0}; uint64_t raw_count = 0; int result = ENV_LIMITED;
    snprintf(path, sizeof(path), "%s/raw_intervals.csv", directory); raw = fopen(path, "w");
    if (raw) { snprintf(path, sizeof(path), "%s/worker_run_summaries.csv", directory); summary = fopen(path, "w"); }
    if (summary) { snprintf(path, sizeof(path), "%s/locality_validation_runs.csv", directory); runs = fopen(path, "w"); }
    provider = worker_evidence_provider_create(); pages = page_candidate_provider_create();
    if (!raw || !summary || !runs || !provider || !pages || !select_cpus(topology->local_node, selected, cpu, cpus, sizeof(cpus))) goto cleanup;
    fprintf(raw, "schema_version,validation_id,run_index,run_kind,pattern,placement,worker_index,interval_index,load_operations_delta,interval_ms,registered_memory_load_rate,metric_name,metric_unit,valid,status\n");
    fprintf(summary, "schema_version,validation_id,run_index,pattern,placement,worker_index,interval_count,window_interval_count,window_count,load_operations_delta,interval_ms,worker_run_median_registered_memory_load_rate,metric_name,metric_unit,valid,status\n");
    fprintf(runs, "schema_version,validation_id,run_index,run_kind,pattern,placement,intensity_percent,workers,memory_mb,duration_ms,startup_discard_ms,seed,local_node,remote_node,requested_memory_node,numa_distance,worker0_tid,worker0_cpu,worker0_package,worker0_core,worker0_siblings,worker1_tid,worker1_cpu,worker1_package,worker1_core,worker1_siblings,affinity_verified,registration_ok,registration_generation,worker0_evidence_seen,worker1_evidence_seen,worker_evidence_ok,start_total_pages,start_queryable_pages,start_expected_pages,start_local_pages,start_remote_pages,start_other_pages,start_unknown_pages,start_status,end_total_pages,end_queryable_pages,end_expected_pages,end_local_pages,end_remote_pages,end_other_pages,end_unknown_pages,end_status,benchmark_exit_code,numa_balancing_original,numa_balancing_disabled,numa_balancing_restored,valid,reason,authoritative\n");
    snprintf(worker_socket, sizeof(worker_socket), "/tmp/awavma-c2b-%ld.sock", (long)getpid()); snprintf(page_socket, sizeof(page_socket), "/tmp/awavma-c2b-pages-%ld.sock", (long)getpid());
    if (!worker_evidence_provider_start(provider, worker_socket) || !page_candidate_provider_start(pages, page_socket, options->duration_ms + 10000U)) goto cleanup;
    P5C2BCell cells[6]; (void)p5_c2b_matrix(cells, 6); unsigned run_index = 0;
    for (unsigned cell = 0; cell < 6 && !stopped; ++cell) for (unsigned run = 0; run < options->warmups + options->measured && !stopped; ++run) {
        bool warmup = run < options->warmups; RunOutput output = {.raw = raw, .exit_code = -1}; benchmark_placement_evidence_t start = {0}, end = {0};
        char placement[768], start_path[776], original[80], siblings0[132], siblings1[132], start_status[80], end_status[80], reason[128];
        int requested = !strcmp(cells[cell].placement, "LOCAL") ? topology->local_node : topology->remote_node;
        if (!numa_begin(&active_numa)) goto cleanup;
        snprintf(original, sizeof(original), "%s", active_numa.original); original[strcspn(original, "\r\n")] = '\0';
        snprintf(placement, sizeof(placement), "%s/run-%02u-placement.csv", directory, run_index); snprintf(start_path, sizeof(start_path), "%s.start", placement);
        pid_t child = launch(options, topology, &cells[cell], run_index, cpus, worker_socket, page_socket, placement);
        bool evidence = child > 0 && collect(provider, pages, child, &cells[cell], run_index, warmup,
            options->discard_ms, &output, options->id);
        bool start_ok = read_placement(start_path, &start) && placement_valid(&start, topology, requested);
        bool end_ok = read_placement(placement, &end) && placement_valid(&end, topology, requested);
        bool restored = numa_restore(&active_numa); bool affinity = output.exit_code == 0;
        bool valid = evidence && start_ok && end_ok && affinity && restored;
        for (unsigned worker = 0; worker < 2 && !warmup; ++worker) {
            P5C2BStatistics window_statistics = {0}; bool unit_valid = valid && p5_c2b_statistics(output.workers[worker].windows, output.workers[worker].window_count, &window_statistics);
            fprintf(summary, "%u,%s,%u,%s,%s,%u,%llu,%u,%zu,%llu,%llu,%.17g,%s,%s,%u,%s\n", P5_C2B_SCHEMA_VERSION,
                options->id, run_index, cells[cell].pattern, cells[cell].placement, worker,
                (unsigned long long)output.workers[worker].intervals, P5_C2B_WINDOW_INTERVALS, output.workers[worker].window_count,
                (unsigned long long)output.workers[worker].operations, (unsigned long long)output.workers[worker].milliseconds,
                unit_valid ? window_statistics.median : 0.0, P5_C2B_METRIC, P5_C2B_METRIC_UNIT, unit_valid, unit_valid ? "OK" : "INVALID_RUN");
            if (unit_valid && unit_counts[cell] < P5_C2B_EXPECTED_UNITS) units[cell][unit_counts[cell]++] = window_statistics.median;
        }
        raw_count += output.raw_count;
        p5_c2a_collector_csv_text(cpu[0].siblings, siblings0, sizeof(siblings0)); p5_c2a_collector_csv_text(cpu[1].siblings, siblings1, sizeof(siblings1));
        p5_c2a_collector_csv_text(start.verification_status, start_status, sizeof(start_status)); p5_c2a_collector_csv_text(end.verification_status, end_status, sizeof(end_status));
        snprintf(reason, sizeof(reason), "%s", valid ? "OK" : !restored ? "NUMA_RESTORE_FAILED" : !start_ok ? "START_PLACEMENT_FAILED" : !end_ok ? "END_PLACEMENT_FAILED" : !affinity ? "BENCHMARK_OR_AFFINITY_FAILED" : "WORKER_EVIDENCE_FAILED");
        fprintf(runs, "%u,%s,%u,%s,%s,%s,100,2,%u,%u,%u,%u,%d,%d,%d,%d,%ld,%d,%d,%d,%s,%ld,%d,%d,%d,%s,%u,%u,%llu,%u,%u,%u,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%s,%d,%s,1,%u,%u,%s,%u\n",
            P5_C2B_SCHEMA_VERSION, options->id, run_index, warmup ? "WARMUP" : "MEASURED", cells[cell].pattern, cells[cell].placement,
            options->memory_mb, options->duration_ms, options->discard_ms, BASE_SEED + run_index, topology->local_node, topology->remote_node, requested, topology->numa_distance,
            (long)output.workers[0].tid, cpu[0].cpu, cpu[0].package, cpu[0].core, siblings0, (long)output.workers[1].tid,
            cpu[1].cpu, cpu[1].package, cpu[1].core, siblings1, affinity, output.registration_generation != 0,
            (unsigned long long)output.registration_generation, output.workers[0].evidence_seen, output.workers[1].evidence_seen, output.evidence_ok,
            start.total_pages, start.queryable_pages, start.expected_node_pages, start.local_pages, start.remote_pages, start.other_pages, start.unknown_pages, start_status,
            end.total_pages, end.queryable_pages, end.expected_node_pages, end.local_pages, end.remote_pages, end.other_pages, end.unknown_pages, end_status,
            output.exit_code, original, restored, valid, reason, options->authoritative);
        fflush(raw); fflush(summary); fflush(runs);
        if (!restored) goto cleanup;
        run_index++;
    }
    if (stopped || write_manifest(options, topology, directory, units, unit_counts, raw_count) != 0) { result = 1; goto cleanup; }
    result = 0;
cleanup:
    if (active_numa.active && !numa_restore(&active_numa)) result = ENV_LIMITED;
    if (provider) worker_evidence_provider_stop(provider);
    if (pages) page_candidate_provider_stop(pages);
    worker_evidence_provider_destroy(provider); page_candidate_provider_destroy(pages);
    if (raw) fclose(raw);
    if (summary) fclose(summary);
    if (runs) fclose(runs);
    return result;
}

int main(int argc, char **argv)
{
    Options options; benchmark_placement_topology_t topology = {0}; char parent[512], directory[640];
    if (!parse_options(argc, argv, &options)) { fprintf(stderr, "usage: %s --id ID [--matrix-b-v1] [--dry-run|--execute] [--smoke] [--output-root PATH] [--benchmark PATH] [--duration-ms N]\n", argv[0]); return 2; }
    printf("status=PLANNED matrix=%s configurations=%u authoritative=%u workers=2 memory_mb=%u intensity_percent=100 duration_ms=%u warmup_runs=%u measured_runs=%u discard_ms=%u seed_base=%u\n",
        P5_C2B_MATRIX_VERSION, 6U * (options.warmups + options.measured), options.authoritative, options.memory_mb, options.duration_ms,
        options.warmups, options.measured, options.discard_ms, BASE_SEED);
    if (benchmark_placement_discover(&topology) != 0) { printf("status=ENV_LIMITED reason=two_permitted_numa_nodes_required\n"); return ENV_LIMITED; }
    printf("status=TOPOLOGY local_node=%d remote_node=%d distance=%d\n", topology.local_node, topology.remote_node, topology.numa_distance);
    if (!options.execute) return 0;
    if (snprintf(parent, sizeof(parent), "%s/p5_locality_validation", options.root) >= (int)sizeof(parent) ||
        snprintf(directory, sizeof(directory), "%s/%s", parent, options.id) >= (int)sizeof(directory)) return 2;
    if ((mkdir(options.root, 0700) != 0 && errno != EEXIST) || (mkdir(parent, 0700) != 0 && errno != EEXIST) || mkdir(directory, 0700) != 0) {
        fprintf(stderr, "status=FAILED reason=output_directory_must_be_new\n"); return 1;
    }
    atexit(emergency_restore); signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler); signal(SIGHUP, signal_handler);
    int result = execute(&options, &topology, directory);
    if (result == 0) printf("status=COLLECTED directory=%s authoritative=%u classification_authority=UNAVAILABLE runtime_authority=DISABLED expected_gain_authority=DISABLED production_migration_authority=DISABLED\n", directory, options.authoritative);
    return result;
}
