#include "thread_confidence.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *header = "schema_version,timestamp_utc,app_id,pid,start_time_ticks,temporal_generation,candidate_tid,candidate_tid_available,candidate_tid_verified,classification,classification_available,placement_relation,placement_available,evidence_status,observation_valid,reason,provenance\n";
static void row(FILE *f, const char *app, int pid, int ticks, int gen, int tid, const char *valid, const char *status)
{
    fprintf(f, "1,t,%s,%d,%d,%d,%d,true,true,HOT,true,REMOTE,true,%s,%s,r,p\n", app, pid, ticks, gen, tid, status, valid);
}
static thread_confidence_result_t evaluate(const char *path, int tid)
{
    thread_confidence_result_t result;
    assert(thread_confidence_evaluate(path, "APP", 10, 20, tid, &result) == 0);
    return result;
}
int main(void)
{
    char dir[] = "/tmp/awavma-thread-confidence-XXXXXX", path[512]; FILE *f;
    thread_confidence_result_t r;
    assert(mkdtemp(dir) != NULL); snprintf(path, sizeof(path), "%s/thread_confidence.csv", dir);
    f = fopen(path, "w"); assert(f != NULL); fputs(header, f); row(f,"APP",10,20,1,30,"true","VALID"); row(f,"APP",10,20,2,30,"true","VALID"); row(f,"APP",10,20,3,30,"true","VALID"); fclose(f);
    r = evaluate(path,30); assert(r.status == THREAD_CONFIDENCE_PASS && r.streak_length == THREAD_CONFIDENCE_REQUIRED_STREAK); /* TC01, TC14, TC19 */
    f = fopen(path, "w"); fputs(header,f); row(f,"APP",10,20,1,30,"true","VALID"); row(f,"APP",10,20,2,30,"true","VALID"); fclose(f);
    r=evaluate(path,30); assert(r.status == THREAD_CONFIDENCE_INSUFFICIENT_STREAK); /* TC02 */
    f=fopen(path,"w"); fputs(header,f); row(f,"APP",10,20,1,30,"true","VALID"); row(f,"APP",10,20,3,30,"true","VALID"); row(f,"APP",10,20,4,30,"true","VALID"); fclose(f);
    r=evaluate(path,30); assert(r.streak_length==2 && r.status==THREAD_CONFIDENCE_GENERATION_GAP); /* TC03, TC18 */
    f=fopen(path,"w"); fputs(header,f); row(f,"APP",10,20,1,30,"true","VALID"); row(f,"APP",10,20,2,30,"false","INVALID_EVIDENCE"); row(f,"APP",10,20,3,30,"true","VALID"); fclose(f);
    r=evaluate(path,30); assert(r.streak_length==1); /* TC04 */
    f=fopen(path,"w"); fputs(header,f); row(f,"APP",10,20,1,31,"true","VALID"); row(f,"APP",10,20,2,30,"true","VALID"); fclose(f);
    r=evaluate(path,30); assert(r.streak_length==1); /* TC05 */
    r = evaluate(path,-1); assert(r.status == THREAD_CONFIDENCE_CANDIDATE_UNAVAILABLE); /* TC12 */
    f=fopen(path,"w"); fputs(header,f); row(f,"APP",10,20,1,30,"false","CANDIDATE_VERIFIED"); fclose(f);
    r=evaluate(path,30); assert(r.status==THREAD_CONFIDENCE_INSUFFICIENT_EVIDENCE && !r.qualified); /* TC13 */
    f=fopen(path,"w"); fputs(header,f); row(f,"APP",10,20,3,30,"true","VALID"); row(f,"APP",10,20,3,30,"false","INVALID_EVIDENCE"); fclose(f);
    r=evaluate(path,30); assert(r.status==THREAD_CONFIDENCE_DUPLICATE_CONFLICT); /* TC09 */
    f=fopen(path,"w"); fputs(header,f); row(f,"APP",10,20,3,30,"true","VALID"); row(f,"APP",10,20,3,30,"true","VALID"); fclose(f);
    r=evaluate(path,30); assert(r.streak_length==1); /* TC08 */
    f=fopen(path,"w"); fputs(header,f); fputs("1,bad\n",f); fclose(f); r=evaluate(path,30); assert(r.status==THREAD_CONFIDENCE_MALFORMED_HISTORY); /* TC11 */
    unlink(path); r=evaluate(path,30); assert(r.status==THREAD_CONFIDENCE_HISTORY_UNAVAILABLE); /* TC15 */
    f=fopen(path,"w"); fputs("other\n",f); fclose(f); r=evaluate(path,30); assert(r.status==THREAD_CONFIDENCE_UNSUPPORTED_SCHEMA); /* TC16 */
    unlink(path); rmdir(dir); puts("thread_confidence_test: PASS"); return 0;
}
