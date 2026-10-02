#define _POSIX_C_SOURCE 200809L

#include "runtime_evidence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");
    if (file == NULL)
        return -1;
    fputs(contents, file);
    return fclose(file);
}

static int has_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "r");
    char buffer[4096] = {0};
    size_t read;

    if (file == NULL)
        return 0;
    read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[read] = '\0';
    return strstr(buffer, text) != NULL;
}

int main(void)
{
    char directory[] = "/tmp/runtime-evidence-XXXXXX";
    char monitor[256], threads[256], classified[256], evidence[256], decision[256];
    int passed, suite_passed;

    if (mkdtemp(directory) == NULL)
        return EXIT_FAILURE;
    snprintf(monitor, sizeof(monitor), "%s/monitor.csv", directory);
    snprintf(threads, sizeof(threads), "%s/threads.csv", directory);
    snprintf(classified, sizeof(classified), "%s/classification.csv", directory);
    snprintf(evidence, sizeof(evidence), "%s/evidence.csv", directory);
    snprintf(decision, sizeof(decision), "%s/decision.csv", directory);
    passed = write_file(monitor,
        "timestamp,elapsed_ms,pid,process_cpu_utilization_percent,interval_minor_faults,interval_major_faults,cache_references,cache_misses,process_numa_pages\n"
        "2026-10-02T00:00:00Z,0,42,80,50,0,1000,100,N0=4 N3=12\n") == 0 &&
        write_file(threads,
        "timestamp,elapsed_ms,pid,tid,cpu,cpu_node,cpu_utilization_percent,state\n"
        "2026-10-02T00:00:00Z,0,42,100,7,3,90,R\n"
        "2026-10-02T00:00:00Z,0,42,101,7,0,80,R\n") == 0 &&
        write_file(classified,
        "timestamp,elapsed_ms,pid,entity_id,score,previous_class,current_class,lambda,window_size,hot_threshold,moderate_threshold,hysteresis,status\n"
        "2026-10-02T00:00:00Z,0,42,process,90,COLD,HOT,0.1,10,100,20,5,CLASSIFIED\n") == 0 &&
        runtime_evidence_write_classifier_input(monitor, "evidence-app", evidence) == 0 &&
         runtime_evidence_write_decision_input(evidence, threads, classified, 99, decision) == 0 &&
        has_text(evidence, "evidence-app,process,") &&
         has_text(decision, ",evidence-app,101,") &&
         has_text(decision, ",3,0,REMOTE,MEASURED_REMOTE_THREAD_TO_MEMORY") &&
         has_text(decision, ",NA,NA,NA,NA,");
    printf("TID01_LOCAL_HOTTER_REMOTE_SELECTED: %s\n", passed ? "PASS" : "FAIL");
    suite_passed = passed;
    passed = write_file(threads,
        "timestamp,elapsed_ms,pid,tid,cpu,cpu_node,cpu_utilization_percent,state\n"
        "2026-10-02T00:00:00Z,0,42,101,7,0,70,R\n"
        "2026-10-02T00:00:00Z,0,42,102,7,0,90,R\n") == 0 &&
        runtime_evidence_write_decision_input(evidence, threads, classified, 99, decision) == 0 &&
        has_text(decision, ",evidence-app,102,");
    printf("TID02_HIGHEST_REMOTE_SELECTED: %s\n", passed ? "PASS" : "FAIL");
    suite_passed = suite_passed && passed;
    passed = write_file(threads,
        "timestamp,elapsed_ms,pid,tid,cpu,cpu_node,cpu_utilization_percent,state\n"
        "2026-10-02T00:00:00Z,0,42,102,7,0,80,R\n"
        "2026-10-02T00:00:00Z,0,42,101,7,0,80,R\n") == 0 &&
        runtime_evidence_write_decision_input(evidence, threads, classified, 99, decision) == 0 &&
        has_text(decision, ",evidence-app,101,");
    printf("TID03_EQUAL_REMOTE_LOWEST_TID: %s\n", passed ? "PASS" : "FAIL");
    suite_passed = suite_passed && passed;
    passed = write_file(threads,
        "timestamp,elapsed_ms,pid,tid,cpu,cpu_node,cpu_utilization_percent,state\n"
        "2026-10-02T00:00:00Z,0,42,101,7,3,90,R\n"
        "2026-10-02T00:00:00Z,0,42,102,7,3,80,R\n") == 0 &&
        runtime_evidence_write_decision_input(evidence, threads, classified, 99, decision) == 0 &&
        has_text(decision, ",LOCAL,NO_REMOTE_THREAD");
    printf("TID04_ALL_LOCAL_NO_CANDIDATE: %s\n", passed ? "PASS" : "FAIL");
    suite_passed = suite_passed && passed;
    unlink(monitor); unlink(threads); unlink(classified); unlink(evidence); unlink(decision); rmdir(directory);
    return suite_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
