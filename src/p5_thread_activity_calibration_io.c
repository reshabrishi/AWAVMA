#include "p5_thread_activity_calibration_io.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RAW_FILE "raw_intervals.csv"
#define WORKER_FILE "worker_run_summaries.csv"
#define MANIFEST_FILE "calibration_manifest.csv"
#define LINE_MAXIMUM 16384U

#define RAW_HEADER "schema_version,calibration_id,run_index,sample_index,worker_index,controlled_profile,intensity_percent,sample_kind,placement_mode,worker_count,memory_mb,load_operations_delta,interval_ms,load_rate_ops_per_ms,metric_name,metric_version,metric_unit,valid,status\n"
#define WORKER_HEADER "schema_version,calibration_id,run_index,worker_index,controlled_profile,intensity_percent,placement_mode,valid_interval_count,valid_window_count,worker_run_median_rate_ops_per_ms,valid,status\n"
#define REPORT_HEADER(prefix) prefix "_worker_run_count," prefix "_window_count," prefix "_statistics_count," prefix "_min," prefix "_max," prefix "_mean," prefix "_median," prefix "_stddev," prefix "_p10," prefix "_p90," prefix "_gap_ratio_available," prefix "_gap_ratio," prefix "_worker_agreement_count," prefix "_worker_agreement_mean," prefix "_worker_agreement_max," prefix "_leave_one_run_out_total," prefix "_leave_one_run_out_in_band," prefix "_leave_one_run_out_fraction"
#define MANIFEST_HEADER "format_version,artifact_root,calibration_id,raw_intervals_file,worker_run_summaries_file,raw_sample_count,worker_run_count,calibration_status,schema_version,architecture,cpu_vendor,cpu_model_name,cpu_family,cpu_model,online_cpu_count,numa_node_count,page_size,hardware_fingerprint,metric_name,metric_version,metric_unit,worker_count,memory_mb,placement_mode,benchmark_seed,benchmark_definition_version,intensity_profile_version,low_intensity_percent,mid_intensity_percent,high_intensity_percent,numa_balancing_state,window_interval_count,minimum_worker_run_units," REPORT_HEADER("low") "," REPORT_HEADER("mid") "," REPORT_HEADER("high") "\n"

static void reason_set(char *out, size_t size, const char *value) { if (out && size) snprintf(out, size, "%s", value); }
static bool text(const char *v, bool empty) { const unsigned char *p = (const unsigned char *)v; if (!v || (!empty && !*v)) return false; for (; *p; ++p) if (*p < 0x20 || *p == ',' || *p == '"') return false; return true; }
static bool profile(P5ThreadActivityProfile v) { return v >= P5_THREAD_ACTIVITY_PROFILE_LOW && v <= P5_THREAD_ACTIVITY_PROFILE_HIGH; }
static bool placement(P5ThreadActivityPlacementMode v) { return v == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL || v == P5_THREAD_ACTIVITY_PLACEMENT_REMOTE; }
static bool kind(P5ThreadActivitySampleKind v) { return v >= P5_THREAD_ACTIVITY_SAMPLE_WARMUP_RUN && v <= P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL; }
static const char *placement_name(P5ThreadActivityPlacementMode v) { return v == P5_THREAD_ACTIVITY_PLACEMENT_LOCAL ? "LOCAL" : v == P5_THREAD_ACTIVITY_PLACEMENT_REMOTE ? "REMOTE" : "UNKNOWN"; }
static const char *kind_name(P5ThreadActivitySampleKind v) { return v == P5_THREAD_ACTIVITY_SAMPLE_WARMUP_RUN ? "WARMUP_RUN" : v == P5_THREAD_ACTIVITY_SAMPLE_WARMUP_INTERVAL ? "WARMUP_INTERVAL" : v == P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL ? "MEASURED_INTERVAL" : "UNKNOWN"; }
static bool parse_profile(const char *v, P5ThreadActivityProfile *out) { if (!strcmp(v, "LOW")) *out = P5_THREAD_ACTIVITY_PROFILE_LOW; else if (!strcmp(v, "MID")) *out = P5_THREAD_ACTIVITY_PROFILE_MID; else if (!strcmp(v, "HIGH")) *out = P5_THREAD_ACTIVITY_PROFILE_HIGH; else return false; return true; }
static bool parse_placement(const char *v, P5ThreadActivityPlacementMode *out) { if (!strcmp(v, "LOCAL")) *out = P5_THREAD_ACTIVITY_PLACEMENT_LOCAL; else if (!strcmp(v, "REMOTE")) *out = P5_THREAD_ACTIVITY_PLACEMENT_REMOTE; else return false; return true; }
static bool parse_kind(const char *v, P5ThreadActivitySampleKind *out) { if (!strcmp(v, "WARMUP_RUN")) *out = P5_THREAD_ACTIVITY_SAMPLE_WARMUP_RUN; else if (!strcmp(v, "WARMUP_INTERVAL")) *out = P5_THREAD_ACTIVITY_SAMPLE_WARMUP_INTERVAL; else if (!strcmp(v, "MEASURED_INTERVAL")) *out = P5_THREAD_ACTIVITY_SAMPLE_MEASURED_INTERVAL; else return false; return true; }
static bool parse_status(const char *v, P5ThreadActivityCalibrationStatus *out) { for (int i = 0; i <= P5_THREAD_ACTIVITY_CALIBRATION_MALFORMED; ++i) if (!strcmp(v, p5_thread_activity_calibration_status_name((P5ThreadActivityCalibrationStatus)i))) { *out = (P5ThreadActivityCalibrationStatus)i; return true; } return false; }

bool p5_thread_activity_calibration_id_valid(const char *id) { size_t n; if (!id || !isalnum((unsigned char)id[0])) return false; n = strlen(id); if (n >= P5_THREAD_ACTIVITY_CALIBRATION_ID_MAX) return false; for (size_t i = 0; i < n; ++i) if (!isalnum((unsigned char)id[i]) && id[i] != '-' && id[i] != '_' && id[i] != '.') return false; return true; }
static bool directory(char *out, size_t size, const char *root, const char *id) { int n = snprintf(out, size, "%s/%s/%s", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME, id); return n >= 0 && (size_t)n < size; }
static bool path(char *out, size_t size, const char *root, const char *id, const char *file) { int n = snprintf(out, size, "%s/%s/%s/%s", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME, id, file); return n >= 0 && (size_t)n < size; }
static bool mkdir_one(const char *p) { return mkdir(p, 0700) == 0 || errno == EEXIST; }
static bool root_parent(const char *root, char *out, size_t size) { int n = snprintf(out, size, "%s/%s", root, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME); return n >= 0 && (size_t)n < size; }

static bool context_safe(const P5ThreadActivityContext *c) { return c && text(c->architecture, false) && text(c->cpu_vendor, false) && text(c->cpu_model_name, false) && text(c->hardware_fingerprint, false) && text(c->metric_name, false) && text(c->metric_unit, false) && text(c->benchmark_definition_version, false) && text(c->intensity_profile_version, false) && placement(c->placement_mode); }
static bool calibration_same(const P5ThreadActivityCalibration *a, const P5ThreadActivityCalibration *b) { return a->status == b->status && !strcmp(a->calibration_id, b->calibration_id) && !memcmp(&a->context, &b->context, sizeof(a->context)) && !memcmp(&a->low, &b->low, sizeof(a->low)) && !memcmp(&a->mid, &b->mid, sizeof(a->mid)) && !memcmp(&a->high, &b->high, sizeof(a->high)); }

static bool write_raw(FILE *f, const P5ThreadActivityRawSample *s, size_t count) {
    if (fputs(RAW_HEADER, f) == EOF) return false;
    for (size_t i = 0; i < count; ++i) if (!profile(s[i].controlled_profile) || !kind(s[i].sample_kind) || !placement(s[i].placement_mode) || !text(s[i].calibration_id, false) || !text(s[i].metric_name, false) || !text(s[i].metric_unit, false) || !text(s[i].reason, true) || fprintf(f, "%u,%s,%llu,%llu,%u,%s,%u,%s,%s,%u,%llu,%llu,%llu,%.17g,%s,%u,%s,%s,%s\n", s[i].schema_version, s[i].calibration_id, (unsigned long long)s[i].run_index, (unsigned long long)s[i].sample_index, s[i].worker_index, p5_thread_activity_profile_name(s[i].controlled_profile), s[i].intensity_percent, kind_name(s[i].sample_kind), placement_name(s[i].placement_mode), s[i].worker_count, (unsigned long long)s[i].memory_mb, (unsigned long long)s[i].load_operations_delta, (unsigned long long)s[i].interval_ms, s[i].load_rate_ops_per_ms, s[i].metric_name, s[i].metric_version, s[i].metric_unit, s[i].valid ? "true" : "false", s[i].reason) < 0) return false;
    return !ferror(f);
}
static bool write_workers(FILE *f, const char *id, const P5ThreadActivityWorkerRun *u, size_t count) {
    if (fputs(WORKER_HEADER, f) == EOF) return false;
    for (size_t i = 0; i < count; ++i) if (!profile(u[i].controlled_profile) || !placement(u[i].placement_mode) || !text(u[i].reason, false) || fprintf(f, "%u,%s,%llu,%u,%s,%u,%s,%zu,%zu,%.17g,%s,%s\n", P5_THREAD_ACTIVITY_SCHEMA_VERSION, id, (unsigned long long)u[i].run_index, u[i].worker_index, p5_thread_activity_profile_name(u[i].controlled_profile), u[i].intensity_percent, placement_name(u[i].placement_mode), u[i].valid_interval_count, u[i].valid_window_count, u[i].worker_run_median_rate_ops_per_ms, u[i].valid ? "true" : "false", u[i].reason) < 0) return false;
    return !ferror(f);
}
static bool write_report(FILE *f, const P5ThreadActivityProfileReport *r) { return fprintf(f, "%zu,%zu,%zu,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%s,%.17g,%zu,%.17g,%.17g,%zu,%zu,%.17g", r->worker_run_count, r->window_count, r->statistics.count, r->statistics.min, r->statistics.max, r->statistics.mean, r->statistics.median, r->statistics.stddev, r->statistics.p10, r->statistics.p90, r->gap_ratio_available ? "true" : "false", r->gap_ratio, r->worker_agreement_count, r->worker_agreement_mean, r->worker_agreement_max, r->leave_one_run_out_total, r->leave_one_run_out_in_band, r->leave_one_run_out_fraction) >= 0; }
static bool write_manifest(FILE *f, const P5ThreadActivityCalibration *a, size_t samples, size_t units) {
    const P5ThreadActivityContext *c = &a->context;
    if (!context_safe(c) || fputs(MANIFEST_HEADER, f) == EOF || fprintf(f, "%u,%s,%s,%s,%s,%zu,%zu,%s,%u,%s,%s,%s,%u,%u,%u,%u,%llu,%s,%s,%u,%s,%u,%llu,%s,%llu,%s,%s,%u,%u,%u,%u,%u,%u,", P5_THREAD_ACTIVITY_ARTIFACT_FORMAT_VERSION, P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME, a->calibration_id, RAW_FILE, WORKER_FILE, samples, units, p5_thread_activity_calibration_status_name(a->status), c->schema_version, c->architecture, c->cpu_vendor, c->cpu_model_name, c->cpu_family, c->cpu_model, c->online_cpu_count, c->numa_node_count, (unsigned long long)c->page_size, c->hardware_fingerprint, c->metric_name, c->metric_version, c->metric_unit, c->worker_count, (unsigned long long)c->memory_mb, placement_name(c->placement_mode), (unsigned long long)c->benchmark_seed, c->benchmark_definition_version, c->intensity_profile_version, c->low_intensity_percent, c->mid_intensity_percent, c->high_intensity_percent, c->numa_balancing_state, c->window_interval_count, c->minimum_worker_run_units) < 0 || !write_report(f, &a->low) || fputc(',', f) == EOF || !write_report(f, &a->mid) || fputc(',', f) == EOF || !write_report(f, &a->high) || fputc('\n', f) == EOF) return false;
    return !ferror(f);
}

static bool fields(char *line, char **out, size_t count) { char *p = line; size_t n = strlen(line); if (!n || line[n - 1] != '\n' || strchr(line, '\r')) return false; line[n - 1] = 0; for (size_t i = 0; i < count; ++i) { out[i] = p; p = strchr(p, ','); if (i + 1 == count) return p == NULL && !strchr(out[i], '"'); if (!p) return false; *p++ = 0; } return false; }
static bool number(const char *v, uint64_t max, uint64_t *out) { char *end; unsigned long long n; if (!v || !*v || *v == '-' || isspace((unsigned char)*v)) return false; errno = 0; n = strtoull(v, &end, 10); if (errno || *end || n > max) return false; *out = n; return true; }
static bool decimal(const char *v, double *out) { char *end; double n; if (!v || !*v || isspace((unsigned char)*v)) return false; errno = 0; n = strtod(v, &end); if (errno || *end || !isfinite(n)) return false; *out = n; return true; }
static bool boolean(const char *v, bool *out) { if (!strcmp(v, "true")) *out = true; else if (!strcmp(v, "false")) *out = false; else return false; return true; }
static bool copy(char *out, size_t size, const char *v, bool empty) { if (!text(v, empty) || strlen(v) >= size) return false; snprintf(out, size, "%s", v); return true; }
static bool header(FILE *f, const char *expected) { char line[LINE_MAXIMUM]; return fgets(line, sizeof(line), f) && !strcmp(line, expected); }

static bool read_raw(const char *name, const char *id, P5ThreadActivityRawSample **out, size_t *out_count) {
    FILE *f = fopen(name, "r"); P5ThreadActivityRawSample *v = NULL; size_t n = 0, cap = 0; char line[LINE_MAXIMUM], *x[19]; uint64_t q[12];
    if (!f || !header(f, RAW_HEADER)) goto bad;
    while (fgets(line, sizeof(line), f)) { P5ThreadActivityRawSample *s; if (!fields(line, x, 19) || strcmp(x[1], id) || !number(x[0], UINT32_MAX, &q[0]) || !number(x[2], UINT64_MAX, &q[1]) || !number(x[3], UINT64_MAX, &q[2]) || !number(x[4], UINT32_MAX, &q[3]) || !number(x[6], UINT32_MAX, &q[4]) || !number(x[9], UINT32_MAX, &q[5]) || !number(x[10], UINT64_MAX, &q[6]) || !number(x[11], UINT64_MAX, &q[7]) || !number(x[12], UINT64_MAX, &q[8]) || !number(x[15], UINT32_MAX, &q[9])) goto bad; if (n == cap) { size_t next = cap ? cap * 2 : 64; void *p = realloc(v, next * sizeof(*v)); if (!p) goto bad; v = p; cap = next; } s = &v[n]; memset(s, 0, sizeof(*s)); if (!parse_profile(x[5], &s->controlled_profile) || !parse_kind(x[7], &s->sample_kind) || !parse_placement(x[8], &s->placement_mode) || !decimal(x[13], &s->load_rate_ops_per_ms) || !boolean(x[17], &s->valid) || !copy(s->calibration_id, sizeof(s->calibration_id), x[1], false) || !copy(s->metric_name, sizeof(s->metric_name), x[14], false) || !copy(s->metric_unit, sizeof(s->metric_unit), x[16], false) || !copy(s->reason, sizeof(s->reason), x[18], true)) goto bad; s->schema_version = (uint32_t)q[0]; s->run_index = q[1]; s->sample_index = q[2]; s->worker_index = (uint32_t)q[3]; s->intensity_percent = (uint32_t)q[4]; s->worker_count = (uint32_t)q[5]; s->memory_mb = q[6]; s->load_operations_delta = q[7]; s->interval_ms = q[8]; s->metric_version = (uint32_t)q[9]; ++n; }
    if (ferror(f) || !n) goto bad;
    fclose(f);
    *out = v;
    *out_count = n;
    return true;
bad: if (f) fclose(f); free(v); return false;
}

static bool report_read(char **x, size_t *i, P5ThreadActivityProfileReport *r) { uint64_t n; bool b; if (!number(x[(*i)++], SIZE_MAX, &n)) return false; r->worker_run_count = n; if (!number(x[(*i)++], SIZE_MAX, &n)) return false; r->window_count = n; if (!number(x[(*i)++], SIZE_MAX, &n)) return false; r->statistics.count = n; if (!decimal(x[(*i)++], &r->statistics.min) || !decimal(x[(*i)++], &r->statistics.max) || !decimal(x[(*i)++], &r->statistics.mean) || !decimal(x[(*i)++], &r->statistics.median) || !decimal(x[(*i)++], &r->statistics.stddev) || !decimal(x[(*i)++], &r->statistics.p10) || !decimal(x[(*i)++], &r->statistics.p90) || !boolean(x[(*i)++], &b) || !decimal(x[(*i)++], &r->gap_ratio)) return false; r->gap_ratio_available=b; if (!number(x[(*i)++], SIZE_MAX, &n)) return false; r->worker_agreement_count=n; if (!decimal(x[(*i)++], &r->worker_agreement_mean) || !decimal(x[(*i)++], &r->worker_agreement_max) || !number(x[(*i)++], SIZE_MAX, &n)) return false; r->leave_one_run_out_total=n; if (!number(x[(*i)++], SIZE_MAX, &n)) return false; r->leave_one_run_out_in_band=n; return decimal(x[(*i)++], &r->leave_one_run_out_fraction); }
static bool read_manifest(const char *name, const char *id, P5ThreadActivityCalibration *a, size_t *samples, size_t *units) {
    FILE *f = fopen(name, "r"); char line[LINE_MAXIMUM], *x[87]; uint64_t n; size_t i = 0;
    if (!f || !header(f, MANIFEST_HEADER) || !fgets(line, sizeof(line), f) || fgetc(f) != EOF || !fields(line, x, 87)) goto bad;
    memset(a, 0, sizeof(*a));
    if (!number(x[i++], UINT32_MAX, &n) || n != P5_THREAD_ACTIVITY_ARTIFACT_FORMAT_VERSION || strcmp(x[i++], P5_THREAD_ACTIVITY_ARTIFACT_ROOT_NAME) || !copy(a->calibration_id, sizeof(a->calibration_id), x[i++], false) || strcmp(a->calibration_id, id) || strcmp(x[i++], RAW_FILE) || strcmp(x[i++], WORKER_FILE) || !number(x[i++], SIZE_MAX, &n)) goto bad;
    *samples=n;
    if (!number(x[i++], SIZE_MAX, &n)) goto bad;
    *units=n;
    if (!parse_status(x[i++], &a->status)) goto bad;
#define N(field,max) do { if (!number(x[i++], max, &n)) goto bad; a->context.field = (__typeof__(a->context.field))n; } while (0)
#define S(field) do { if (!copy(a->context.field, sizeof(a->context.field), x[i++], false)) goto bad; } while (0)
    N(schema_version, UINT32_MAX); S(architecture); S(cpu_vendor); S(cpu_model_name); N(cpu_family, UINT32_MAX); N(cpu_model, UINT32_MAX); N(online_cpu_count, UINT32_MAX); N(numa_node_count, UINT32_MAX); N(page_size, UINT64_MAX); S(hardware_fingerprint); S(metric_name); N(metric_version, UINT32_MAX); S(metric_unit); N(worker_count, UINT32_MAX); N(memory_mb, UINT64_MAX); if (!parse_placement(x[i++], &a->context.placement_mode)) goto bad; N(benchmark_seed, UINT64_MAX); S(benchmark_definition_version); S(intensity_profile_version); N(low_intensity_percent, UINT32_MAX); N(mid_intensity_percent, UINT32_MAX); N(high_intensity_percent, UINT32_MAX); N(numa_balancing_state, UINT32_MAX); N(window_interval_count, UINT32_MAX); N(minimum_worker_run_units, UINT32_MAX);
#undef N
#undef S
    if (!report_read(x, &i, &a->low) || !report_read(x, &i, &a->mid) || !report_read(x, &i, &a->high) || i != 87) goto bad;
    fclose(f); return true;
bad: if (f) fclose(f); return false;
}

static bool exact_file(const char *name, bool (*writer)(FILE *, void *), void *arg) { FILE *expected = tmpfile(), *actual = fopen(name, "r"); int a, b; bool ok = expected && actual && writer(expected, arg) && fflush(expected) == 0; if (ok) { rewind(expected); do { a = fgetc(expected); b = fgetc(actual); } while (a != EOF && a == b); ok = a == b; } if (expected) fclose(expected); if (actual) fclose(actual); return ok; }
static bool worker_rows_parse(const char *name, const char *id) { FILE *f=fopen(name,"r"); char line[LINE_MAXIMUM], *x[12]; uint64_t n; double d; bool b; P5ThreadActivityProfile p; P5ThreadActivityPlacementMode m; if (!f || !header(f,WORKER_HEADER)) goto bad; while (fgets(line,sizeof(line),f)) if (!fields(line,x,12) || !number(x[0],UINT32_MAX,&n) || n != P5_THREAD_ACTIVITY_SCHEMA_VERSION || strcmp(x[1],id) || !number(x[2],UINT64_MAX,&n) || !number(x[3],UINT32_MAX,&n) || !parse_profile(x[4],&p) || !number(x[5],UINT32_MAX,&n) || !parse_placement(x[6],&m) || !number(x[7],SIZE_MAX,&n) || !number(x[8],SIZE_MAX,&n) || !decimal(x[9],&d) || !boolean(x[10],&b) || !text(x[11],false)) goto bad; if (ferror(f)) goto bad; fclose(f); return true; bad: if (f) fclose(f); return false; }
static bool write_file(const char *name, bool (*writer)(FILE *, void *), void *arg) { FILE *f=fopen(name,"w"); if (!f) return false; if (!writer(f,arg)) { fclose(f); return false; } return fclose(f) == 0; }
typedef struct { const P5ThreadActivityRawSample *samples; size_t count; } RawWrite;
typedef struct { const char *id; const P5ThreadActivityWorkerRun *units; size_t count; } WorkerWrite;
typedef struct { const P5ThreadActivityCalibration *calibration; size_t samples, units; } ManifestWrite;
static bool raw_writer(FILE *f, void *a) { RawWrite *v=a; return write_raw(f,v->samples,v->count); }
static bool worker_writer(FILE *f, void *a) { WorkerWrite *v=a; return write_workers(f,v->id,v->units,v->count); }
static bool manifest_writer(FILE *f, void *a) { ManifestWrite *v=a; return write_manifest(f,v->calibration,v->samples,v->units); }

int p5_thread_activity_calibration_write_artifacts(const char *root, const P5ThreadActivityContext *context, const char *id, const P5ThreadActivityRawSample *samples, size_t sample_count, const P5ThreadActivityCalibration *calibration, char *out_reason, size_t reason_size) {
    P5ThreadActivityCalibration rebuilt; P5ThreadActivityWorkerRun *units=NULL; size_t unit_count=0; char why[P5_THREAD_ACTIVITY_REASON_MAX], parent[4096]={0}, final[4096]={0}, staging[4096]={0}, raw[4096]={0}, workers[4096]={0}, manifest[4096]={0}; RawWrite rw; WorkerWrite ww; ManifestWrite mw;
    if (!root || !*root || !p5_thread_activity_calibration_id_valid(id) || !context_safe(context) || !samples || !sample_count || !calibration || !directory(final,sizeof(final),root,id) || access(final,F_OK) == 0 || !root_parent(root,parent,sizeof(parent)) || !mkdir_one(root) || !mkdir_one(parent) || snprintf(staging,sizeof(staging),"%s/.%s.staging.%ld",parent,id,(long)getpid()) >= (int)sizeof(staging) || mkdir(staging,0700) != 0 || p5_thread_activity_calibration_build(context,id,samples,sample_count,&rebuilt,why,sizeof(why)) != 0 || !calibration_same(calibration,&rebuilt) || p5_thread_activity_calibration_derive_worker_runs(context,id,samples,sample_count,&units,&unit_count,why,sizeof(why)) != 0) goto bad;
    if (snprintf(raw,sizeof(raw),"%s/%s",staging,RAW_FILE) >= (int)sizeof(raw) || snprintf(workers,sizeof(workers),"%s/%s",staging,WORKER_FILE) >= (int)sizeof(workers) || snprintf(manifest,sizeof(manifest),"%s/%s",staging,MANIFEST_FILE) >= (int)sizeof(manifest)) goto bad;
    rw=(RawWrite){samples,sample_count}; ww=(WorkerWrite){id,units,unit_count}; mw=(ManifestWrite){&rebuilt,sample_count,unit_count};
    if (!write_file(raw,raw_writer,&rw) || !write_file(workers,worker_writer,&ww) || !write_file(manifest,manifest_writer,&mw)) goto bad;
    if (rename(staging,final) != 0) goto bad;
    free(units);
    reason_set(out_reason,reason_size,"written");
    return 0;
bad:
    free(units);
    if (staging[0]) {
        if (strlen(staging) + sizeof(RAW_FILE) < sizeof(raw)) { (void)snprintf(raw,sizeof(raw),"%s/%s",staging,RAW_FILE); remove(raw); }
        if (strlen(staging) + sizeof(WORKER_FILE) < sizeof(workers)) { (void)snprintf(workers,sizeof(workers),"%s/%s",staging,WORKER_FILE); remove(workers); }
        if (strlen(staging) + sizeof(MANIFEST_FILE) < sizeof(manifest)) { (void)snprintf(manifest,sizeof(manifest),"%s/%s",staging,MANIFEST_FILE); remove(manifest); }
        rmdir(staging);
    }
    reason_set(out_reason,reason_size,"artifact_write_failed");
    return -1;
}

int p5_thread_activity_calibration_load_artifacts(const char *root, const char *id, P5ThreadActivityCalibration *calibration, P5ThreadActivityRawSample **samples, size_t *sample_count, char *out_reason, size_t reason_size) {
    char raw[4096], workers[4096], manifest[4096], why[P5_THREAD_ACTIVITY_REASON_MAX]; P5ThreadActivityCalibration recorded, rebuilt; P5ThreadActivityRawSample *loaded=NULL; P5ThreadActivityWorkerRun *units=NULL; size_t count=0, expected_samples=0, unit_count=0, expected_units=0; RawWrite rw; WorkerWrite ww; ManifestWrite mw;
    if (!root || !*root || !calibration || !samples || !sample_count || !p5_thread_activity_calibration_id_valid(id) || !path(raw,sizeof(raw),root,id,RAW_FILE) || !path(workers,sizeof(workers),root,id,WORKER_FILE) || !path(manifest,sizeof(manifest),root,id,MANIFEST_FILE) || !read_manifest(manifest,id,&recorded,&expected_samples,&expected_units) || !read_raw(raw,id,&loaded,&count) || count != expected_samples || p5_thread_activity_calibration_derive_worker_runs(&recorded.context,id,loaded,count,&units,&unit_count,why,sizeof(why)) != 0 || unit_count != expected_units || p5_thread_activity_calibration_build(&recorded.context,id,loaded,count,&rebuilt,why,sizeof(why)) != 0 || !calibration_same(&recorded,&rebuilt)) goto bad;
    rw=(RawWrite){loaded,count}; ww=(WorkerWrite){id,units,unit_count}; mw=(ManifestWrite){&rebuilt,count,unit_count}; if (!worker_rows_parse(workers,id) || !exact_file(raw,raw_writer,&rw) || !exact_file(workers,worker_writer,&ww) || !exact_file(manifest,manifest_writer,&mw)) goto bad;
    free(units); *calibration=rebuilt; *samples=loaded; *sample_count=count; reason_set(out_reason,reason_size,"loaded"); return 0;
bad: free(units); free(loaded); reason_set(out_reason,reason_size,"invalid_artifact"); return -1;
}
