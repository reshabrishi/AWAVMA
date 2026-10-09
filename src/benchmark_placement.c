#include "benchmark_placement.h"

#include <errno.h>
#include <limits.h>
#include <linux/mempolicy.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PLACEMENT_MAX_NODES 64U
#define PLACEMENT_QUERY_BATCH 4096U

static bool parse_cpu_list(const char *text, cpu_set_t *set)
{
    const char *cursor = text;
    CPU_ZERO(set);
    while (*cursor != '\0' && *cursor != '\n') {
        char *end;
        long first = strtol(cursor, &end, 10), last = first;
        if (end == cursor || first < 0 || first >= CPU_SETSIZE) return false;
        if (*end == '-') {
            last = strtol(end + 1, &end, 10);
            if (last < first || last >= CPU_SETSIZE) return false;
        }
        for (long cpu = first; cpu <= last; cpu++) CPU_SET((int)cpu, set);
        if (*end == ',') cursor = end + 1;
        else if (*end == '\0' || *end == '\n') break;
        else return false;
    }
    return true;
}

const char *benchmark_placement_mode_name(benchmark_placement_mode_t mode)
{
    return mode == BENCHMARK_PLACEMENT_LOCAL ? "local" :
           mode == BENCHMARK_PLACEMENT_REMOTE ? "remote" : "default";
}

bool benchmark_placement_select(const benchmark_placement_node_t *nodes, size_t count,
                                 benchmark_placement_topology_t *topology)
{
    int local = -1, remote = -1, distance = -1;
    unsigned permitted_node_count = 0;
    if (nodes == NULL || topology == NULL) return false;
    for (size_t index = 0; index < count; index++) {
        if (nodes[index].permitted_cpus > 0) permitted_node_count++;
        if (nodes[index].permitted_cpus > 0 && (local < 0 || nodes[index].node < local)) local = nodes[index].node;
    }
    if (local < 0) return false;
    for (size_t index = 0; index < count; index++)
        if (nodes[index].permitted_cpus > 0 && nodes[index].node != local &&
            (nodes[index].distance_from_local > distance ||
             (nodes[index].distance_from_local == distance &&
              (remote < 0 || nodes[index].node < remote)))) {
            remote = nodes[index].node;
            distance = nodes[index].distance_from_local;
        }
    topology->local_node = local;
    topology->remote_node = remote;
    topology->numa_distance = distance;
    topology->permitted_node_count = permitted_node_count;
    return remote >= 0;
}

int benchmark_placement_discover(benchmark_placement_topology_t *topology)
{
    benchmark_placement_node_t nodes[PLACEMENT_MAX_NODES] = {{0}};
    cpu_set_t allowed;
    size_t count = 0;
    char path[128], line[4096];
    if (topology == NULL || sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return -1;
    for (unsigned node = 0; node < PLACEMENT_MAX_NODES; node++) {
        FILE *file;
        cpu_set_t cpus;
        unsigned permitted = 0;
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/cpulist", node);
        file = fopen(path, "r");
        if (file == NULL) continue;
        if (fgets(line, sizeof(line), file) == NULL || !parse_cpu_list(line, &cpus)) { fclose(file); return -1; }
        fclose(file);
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, &cpus) && CPU_ISSET(cpu, &allowed)) permitted++;
        nodes[count++] = (benchmark_placement_node_t){.node = (int)node, .permitted_cpus = permitted};
    }
    if (count < 2) return 1;
    int local = -1;
    for (size_t index = 0; index < count; index++) if (nodes[index].permitted_cpus && (local < 0 || nodes[index].node < local)) local = nodes[index].node;
    if (local < 0) return 1;
    for (size_t index = 0; index < count; index++) {
        FILE *file;
        int distance;
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/distance", nodes[index].node);
        file = fopen(path, "r");
        if (file == NULL) return -1;
        for (int column = 0; column <= local; column++) if (fscanf(file, "%d", &distance) != 1) { fclose(file); return -1; }
        fclose(file);
        nodes[index].distance_from_local = distance;
    }
    return benchmark_placement_select(nodes, count, topology) ? 0 : 1;
}

static int set_policy(int node)
{
#ifdef SYS_set_mempolicy
    unsigned long mask = node < (int)(sizeof(mask) * CHAR_BIT) ? 1UL << node : 0;
    if (node >= 0 && mask == 0) return -1;
    return syscall(SYS_set_mempolicy, node < 0 ? MPOL_DEFAULT : MPOL_BIND,
                   node < 0 ? NULL : &mask, node < 0 ? 0UL : sizeof(mask) * CHAR_BIT);
#else
    (void)node; errno = ENOSYS; return -1;
#endif
}

static bool default_policy_verified(void)
{
#ifdef SYS_get_mempolicy
    int mode = -1;
    return syscall(SYS_get_mempolicy, &mode, NULL, 0UL, NULL, 0UL) == 0 && mode == MPOL_DEFAULT;
#else
    return false;
#endif
}

static void touch_pages(void *allocation, size_t bytes, size_t page_size)
{
    volatile unsigned char *memory = allocation;
    for (size_t offset = 0; offset < bytes; offset += page_size) memory[offset] = 0;
}

static int query_pages(void *allocation, size_t bytes, size_t page_size,
                       const benchmark_placement_topology_t *topology,
                       int expected, benchmark_placement_evidence_t *evidence)
{
    void *pages[PLACEMENT_QUERY_BATCH];
    int status[PLACEMENT_QUERY_BATCH];
    size_t total = bytes / page_size;
    int dominant_count = -1;
    int counts[PLACEMENT_MAX_NODES] = {0};
    for (size_t base = 0; base < total; base += PLACEMENT_QUERY_BATCH) {
        size_t count = total - base < PLACEMENT_QUERY_BATCH ? total - base : PLACEMENT_QUERY_BATCH;
        for (size_t index = 0; index < count; index++) pages[index] = (char *)allocation + (base + index) * page_size;
#ifdef SYS_move_pages
        if (syscall(SYS_move_pages, 0, count, pages, NULL, status, 0) < 0) return errno == EPERM || errno == EACCES || errno == ENOSYS ? 1 : -1;
#else
        return 1;
#endif
        for (size_t index = 0; index < count; index++) {
            if (status[index] < 0) { evidence->unknown_pages++; continue; }
            evidence->queryable_pages++;
            if (status[index] == expected) evidence->expected_node_pages++;
            if (status[index] == topology->local_node) evidence->local_pages++;
            else if (status[index] == topology->remote_node) evidence->remote_pages++;
            else evidence->other_pages++;
            if (status[index] < (int)PLACEMENT_MAX_NODES && ++counts[status[index]] > dominant_count) {
                dominant_count = counts[status[index]]; evidence->observed_dominant_node = status[index];
            }
        }
    }
    return 0;
}

int benchmark_placement_prepare(benchmark_placement_mode_t mode,
                                const benchmark_placement_topology_t *topology,
                                void *allocation, size_t bytes,
                                benchmark_placement_evidence_t *evidence)
{
    long page_size = sysconf(_SC_PAGESIZE);
    int expected = -1, result;
    if (topology == NULL || allocation == NULL || evidence == NULL || page_size <= 0 ||
        bytes == 0 || bytes % (size_t)page_size != 0) return -1;
    memset(evidence, 0, sizeof(*evidence));
    evidence->mode = mode; evidence->local_node = topology->local_node;
    evidence->requested_memory_node = mode == BENCHMARK_PLACEMENT_LOCAL ? topology->local_node :
                                      mode == BENCHMARK_PLACEMENT_REMOTE ? topology->remote_node : -1;
    evidence->numa_distance = topology->numa_distance; evidence->total_pages = bytes / (size_t)page_size;
    evidence->observed_dominant_node = -1;
    if (mode != BENCHMARK_PLACEMENT_DEFAULT && set_policy(evidence->requested_memory_node) != 0) {
        snprintf(evidence->verification_status, sizeof(evidence->verification_status), "ENV_LIMITED");
        snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "temporary memory policy unavailable");
        return 1;
    }
    touch_pages(allocation, bytes, (size_t)page_size);
    if (mode != BENCHMARK_PLACEMENT_DEFAULT) {
        if (set_policy(-1) != 0) {
            snprintf(evidence->verification_status, sizeof(evidence->verification_status), "FAIL");
            snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "memory policy restoration failed");
            return -1;
        }
        if (!default_policy_verified()) {
            snprintf(evidence->verification_status, sizeof(evidence->verification_status), "ENV_LIMITED");
            snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "memory policy restoration could not be verified");
            return 1;
        }
        evidence->memory_policy_restored = true;
        expected = evidence->requested_memory_node;
    } else evidence->memory_policy_restored = true;
    result = query_pages(allocation, bytes, (size_t)page_size, topology, expected, evidence);
    if (result != 0) {
        if (mode == BENCHMARK_PLACEMENT_DEFAULT) {
            snprintf(evidence->verification_status, sizeof(evidence->verification_status), "OBSERVATION_UNAVAILABLE");
            snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "physical page query unavailable");
            return 0;
        }
        snprintf(evidence->verification_status, sizeof(evidence->verification_status), result > 0 ? "ENV_LIMITED" : "FAIL");
        snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "physical page query failed");
        return result;
    }
    if (mode == BENCHMARK_PLACEMENT_DEFAULT) {
        snprintf(evidence->verification_status, sizeof(evidence->verification_status), "OBSERVED");
        snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "default placement observed");
        return 0;
    }
    if (evidence->queryable_pages != evidence->total_pages || evidence->expected_node_pages != evidence->total_pages ||
        evidence->other_pages != 0 || evidence->unknown_pages != 0) {
        snprintf(evidence->verification_status, sizeof(evidence->verification_status), "FAIL");
        snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "initial page placement differs from requested node");
        return -1;
    }
    snprintf(evidence->verification_status, sizeof(evidence->verification_status), "PASS");
    snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "all pages verified on requested node");
    return 0;
}

int benchmark_placement_verify(const benchmark_placement_topology_t *topology, void *allocation,
                               size_t bytes, benchmark_placement_evidence_t *evidence)
{
    long page_size = sysconf(_SC_PAGESIZE);
    int result;
    if (topology == NULL || allocation == NULL || evidence == NULL || page_size <= 0 || bytes == 0 ||
        bytes % (size_t)page_size != 0 || evidence->mode == BENCHMARK_PLACEMENT_DEFAULT) return -1;
    evidence->queryable_pages = evidence->expected_node_pages = evidence->local_pages = 0;
    evidence->remote_pages = evidence->other_pages = evidence->unknown_pages = 0;
    evidence->observed_dominant_node = -1;
    result = query_pages(allocation, bytes, (size_t)page_size, topology, evidence->requested_memory_node, evidence);
    if (result != 0 || evidence->queryable_pages != evidence->total_pages ||
        evidence->expected_node_pages != evidence->total_pages || evidence->other_pages != 0 ||
        evidence->unknown_pages != 0) {
        snprintf(evidence->verification_status, sizeof(evidence->verification_status), result > 0 ? "ENV_LIMITED" : "FAIL");
        snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "end-of-run page placement differs from requested node");
        return result == 0 ? -1 : result;
    }
    snprintf(evidence->verification_status, sizeof(evidence->verification_status), "PASS");
    snprintf(evidence->verification_reason, sizeof(evidence->verification_reason), "start and end placement verified on requested node");
    return 0;
}

int benchmark_placement_write(const char *path, const benchmark_placement_evidence_t *evidence)
{
    char temporary[PATH_MAX];
    FILE *file;
    if (path == NULL || evidence == NULL || snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return -1;
    file = fopen(temporary, "w");
    if (file == NULL) return -1;
    fprintf(file, "schema_version,placement_mode,local_node,requested_memory_node,numa_distance,total_pages,queryable_pages,expected_node_pages,local_pages,remote_pages,other_pages,unknown_pages,expected_node_ratio,observed_dominant_node,verification_status,verification_reason,memory_policy_restored\n");
    fprintf(file, "1,%s,%d,%d,%d,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.9f,%d,%s,%s,%s\n",
            benchmark_placement_mode_name(evidence->mode), evidence->local_node, evidence->requested_memory_node,
            evidence->numa_distance, evidence->total_pages, evidence->queryable_pages,
            evidence->expected_node_pages, evidence->local_pages, evidence->remote_pages,
            evidence->other_pages, evidence->unknown_pages,
            evidence->total_pages == 0 ? 0.0 : (double)evidence->expected_node_pages / evidence->total_pages,
            evidence->observed_dominant_node, evidence->verification_status, evidence->verification_reason,
            evidence->memory_policy_restored ? "true" : "false");
    return fclose(file) == 0 && rename(temporary, path) == 0 ? 0 : -1;
}
