#include "thread_confidence_history.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static thread_confidence_history_observation_t observation(uint64_t generation)
{
    return (thread_confidence_history_observation_t){
        .timestamp_utc = "2026-10-07T00:00:00Z", .app_id = "APP_TEST", .pid = 100,
        .start_time_ticks = 200, .temporal_generation = generation, .candidate_tid = -1,
        .candidate_tid_available = false, .candidate_tid_verified = false, .classification = "NA",
        .classification_available = false, .placement_relation = "NA", .placement_available = false,
        .evidence_status = THREAD_CONFIDENCE_HISTORY_CANDIDATE_UNAVAILABLE,
        .observation_valid = false, .reason = "canonical_candidate_tid_unavailable",
        .provenance = "CURRENT_RUNTIME_PROCESS_CYCLE"};
}

int main(void)
{
    char directory[] = "/tmp/awavma-temporal-history-XXXXXX";
    char path[512], malformed[512], contents[4096] = {0};
    thread_confidence_history_observation_t row;
    uint64_t generation;
    FILE *file;

    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/thread_confidence.csv", directory);
    for (generation = 1; generation <= 3; generation++) {
        row = observation(generation);
        assert(thread_confidence_history_persist(path, &row) == 0);
    }
    assert(thread_confidence_history_next_generation(path, "APP_TEST", 100, 200, &generation) == 0 && generation == 4);
    row = observation(3);
    assert(thread_confidence_history_persist(path, &row) == 0);
    row.reason = "conflicting_duplicate";
    assert(thread_confidence_history_persist(path, &row) != 0);
    row = observation(4);
    row.evidence_status = THREAD_CONFIDENCE_HISTORY_VALID;
    row.observation_valid = true;
    assert(thread_confidence_history_persist(path, &row) != 0);
    assert(thread_confidence_history_next_generation(path, "APP_TEST", 100, 201, &generation) == 0 && generation == 1);
    assert(thread_confidence_history_next_generation(path, "APP_OTHER", 100, 200, &generation) == 0 && generation == 1);
    file = fopen(path, "r");
    assert(file != NULL && fread(contents, 1, sizeof(contents) - 1, file) > 0 && fclose(file) == 0);
    assert(strstr(contents, "candidate_tid_available") != NULL && strstr(contents, ",NA,false,false,") != NULL);
    assert(strstr(contents, "0x") == NULL && strstr(contents, "address") == NULL);
    snprintf(malformed, sizeof(malformed), "%s/malformed.csv", directory);
    file = fopen(malformed, "w");
    assert(file != NULL);
    fputs("schema_version,timestamp_utc,app_id,pid,start_time_ticks,temporal_generation,candidate_tid,candidate_tid_available,candidate_tid_verified,classification,classification_available,placement_relation,placement_available,evidence_status,observation_valid,reason,provenance\n1,bad\n", file);
    assert(fclose(file) == 0);
    assert(thread_confidence_history_next_generation(malformed, "APP_TEST", 100, 200, &generation) != 0);
    file = fopen(malformed, "w");
    assert(file != NULL);
    fputs("schema_version,timestamp_utc,app_id,pid,start_time_ticks,temporal_generation,candidate_tid,candidate_tid_available,candidate_tid_verified,classification,classification_available,placement_relation,placement_available,evidence_status,observation_valid,reason,provenance\n1,2026-10-07T00:00:00Z,APP_TEST,100,200,9,NA,false,false,NA,false,NA,false,NOT_A_STATUS,false,invalid,CURRENT_RUNTIME_PROCESS_CYCLE\n", file);
    assert(fclose(file) == 0);
    assert(thread_confidence_history_next_generation(malformed, "APP_TEST", 100, 200, &generation) != 0);
    row = observation(1);
    assert(thread_confidence_history_persist("/missing/thread_confidence.csv", &row) != 0);
    assert(unlink(path) == 0 && unlink(malformed) == 0 && rmdir(directory) == 0);
    printf("thread_confidence_history_test: PASS\n");
    return 0;
}
