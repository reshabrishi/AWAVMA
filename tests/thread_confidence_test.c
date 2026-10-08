#include "thread_confidence.h"
#include "migration_types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *header = "schema_version,timestamp_utc,app_id,pid,start_time_ticks,temporal_generation,candidate_tid,candidate_tid_available,candidate_tid_verified,classification,classification_available,placement_relation,placement_available,evidence_status,observation_valid,reason,provenance\n";

static void write_header(FILE *file) { assert(fputs(header, file) >= 0); }
static void row(FILE *file, const char *app, unsigned long ticks, unsigned long generation,
                int tid, const char *available, const char *verified, const char *classification,
                const char *class_available, const char *placement, const char *placement_available,
                const char *status, const char *valid)
{
    assert(fprintf(file, "1,t,%s,10,%lu,%lu,%d,%s,%s,%s,%s,%s,%s,%s,%s,r,p\n", app, ticks,
                   generation, tid, available, verified, classification, class_available, placement,
                   placement_available, status, valid) > 0);
}
static void qualifying(FILE *file, const char *app, unsigned long ticks, unsigned long generation, int tid)
{ row(file, app, ticks, generation, tid, "true", "true", "HOT", "true", "REMOTE", "true", "VALID", "true"); }
static thread_confidence_result_t evaluate(const char *path, const char *app, unsigned long ticks, int tid)
{
    thread_confidence_result_t result;
    assert(thread_confidence_evaluate(path, app, 10, ticks, tid, &result) == 0);
    return result;
}
static FILE *reset(const char *path) { FILE *file = fopen(path, "w"); assert(file != NULL); write_header(file); return file; }
static void passed(unsigned number) { printf("TC%02u: PASS\n", number); }

int main(void)
{
    char directory[] = "/tmp/awavma-thread-confidence-XXXXXX", path[512];
    FILE *file; thread_confidence_result_t result;
    assert(mkdtemp(directory) != NULL); snprintf(path, sizeof(path), "%s/thread_confidence.csv", directory);

    file = reset(path); qualifying(file,"APP",20,1,30); qualifying(file,"APP",20,2,30); qualifying(file,"APP",20,3,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.status == THREAD_CONFIDENCE_PASS && result.streak_length == 3); passed(1);
    file = reset(path); qualifying(file,"APP",20,1,30); qualifying(file,"APP",20,2,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.status == THREAD_CONFIDENCE_INSUFFICIENT_STREAK && result.streak_length == 2); passed(2);
    file = reset(path); qualifying(file,"APP",20,1,30); qualifying(file,"APP",20,3,30); qualifying(file,"APP",20,4,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.streak_length == 2 && !result.qualified); passed(3);
    file = reset(path); qualifying(file,"APP",20,1,30); row(file,"APP",20,2,30,"true","true","HOT","true","REMOTE","true","INVALID_EVIDENCE","false"); qualifying(file,"APP",20,3,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.streak_length == 1 && !result.qualified); passed(4);
    file = reset(path); qualifying(file,"APP",20,1,30); qualifying(file,"APP",20,2,31); qualifying(file,"APP",20,3,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.streak_length == 1); passed(5);
    file = reset(path); qualifying(file,"APP",21,1,30); qualifying(file,"APP",20,1,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.latest_temporal_generation == 1 && result.streak_length == 1); passed(6);
    file = reset(path); qualifying(file,"OTHER",20,1,30); qualifying(file,"APP",20,1,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.latest_temporal_generation == 1 && result.streak_length == 1); passed(7);
    file = reset(path); qualifying(file,"APP",20,1,30); qualifying(file,"APP",20,1,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.streak_length == 1); passed(8);
    file = reset(path); qualifying(file,"APP",20,3,30); row(file,"APP",20,3,30,"true","true","COLD","true","REMOTE","true","VALID","true"); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.status == THREAD_CONFIDENCE_DUPLICATE_CONFLICT && !result.qualified); passed(9);
    file = reset(path); qualifying(file,"APP",20,8,30); qualifying(file,"APP",20,4,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.latest_temporal_generation == 8 && result.streak_length == 1); passed(10);
    file = reset(path); qualifying(file,"APP",20,3,30); fputs("1,bad\n", file); qualifying(file,"APP",20,5,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.status == THREAD_CONFIDENCE_MALFORMED_HISTORY && !result.qualified); passed(11);
    result = evaluate(path,"APP",20,-1); assert(result.status == THREAD_CONFIDENCE_CANDIDATE_UNAVAILABLE && !result.qualified); passed(12);
    file = reset(path); row(file,"APP",20,1,30,"true","true","NA","false","NA","false","CANDIDATE_VERIFIED","false"); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.status == THREAD_CONFIDENCE_INSUFFICIENT_EVIDENCE && !result.qualified); passed(13);
    file = reset(path); qualifying(file,"APP",20,1,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.available && result.streak_length == 1); passed(14);
    unlink(path); result = evaluate(path,"APP",20,30); assert(result.status == THREAD_CONFIDENCE_HISTORY_UNAVAILABLE && !result.qualified); passed(15);
    file = fopen(path,"w"); assert(file != NULL); fputs("schema_version,other\n",file); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.status == THREAD_CONFIDENCE_UNSUPPORTED_SCHEMA && !result.qualified); passed(16);
    file = reset(path); qualifying(file,"APP",20,1,30); qualifying(file,"APP",20,2,30); row(file,"APP",20,3,30,"true","true","HOT","true","REMOTE","true","INVALID_EVIDENCE","false"); qualifying(file,"APP",20,4,30); qualifying(file,"APP",20,5,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.streak_length == 2 && !result.qualified); passed(17);
    file = reset(path); qualifying(file,"APP",20,1,30); qualifying(file,"APP",20,3,30); qualifying(file,"APP",20,4,30); fclose(file);
    result = evaluate(path,"APP",20,30); assert(result.streak_length == 2 && result.status != THREAD_CONFIDENCE_PASS); passed(18);
    assert(THREAD_CONFIDENCE_REQUIRED_STREAK == 3U); passed(19);
    {
        MigrationRequest request = {.tid = 777, .destination_numa_node = 9};
        MigrationRequest before = request;
        file = reset(path); qualifying(file,"APP",20,1,30); qualifying(file,"APP",20,2,30); qualifying(file,"APP",20,3,30); fclose(file);
        result = evaluate(path,"APP",20,30);
        assert(result.status == THREAD_CONFIDENCE_PASS && result.qualified);
        assert(memcmp(&request, &before, sizeof(request)) == 0);
    }
    passed(20);
    unlink(path); rmdir(directory); puts("Thread confidence tests: PASS"); return 0;
}
