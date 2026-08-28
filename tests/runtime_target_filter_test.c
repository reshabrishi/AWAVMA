#include "runtime_target_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report(const char *id, int passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
}

int main(void)
{
    runtime_target_filter_t filter;
    application_manager_record_t record;
    int passed = 1;

    runtime_target_filter_init(&filter);
    passed = runtime_target_filter_add_identity(&filter, 4101, 1001) == 0 &&
             runtime_target_filter_add_identity(&filter, 4101, 1001) == 0 &&
             runtime_target_filter_add_identity(&filter, 4102, 1002) == 0 && filter.count == 2;
    report("RTF01", passed);
    memset(&record, 0, sizeof(record));
    record.pid = 4101;
    record.start_time_ticks = 1001;
    passed = passed && runtime_target_filter_matches(&record, &filter);
    report("RTF02", passed);
    record.start_time_ticks = 2001;
    passed = passed && !runtime_target_filter_matches(&record, &filter);
    report("RTF03", passed);
    record.pid = 4103;
    record.start_time_ticks = 1001;
    passed = passed && !runtime_target_filter_matches(&record, &filter);
    report("RTF04", passed);
    runtime_target_filter_cleanup(&filter);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
