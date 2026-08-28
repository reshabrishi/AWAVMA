#define _POSIX_C_SOURCE 200809L

#include "application_discovery.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void report_result(const char *id, int passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
    if (!passed)
        failures++;
}

static application_discovery_t *create_discovery(pid_t excluded_pid,
                                                 const char *proc_root)
{
    application_discovery_config_t config;
    application_discovery_t *discovery = application_discovery_create();

    application_discovery_config_default(&config);
    config.excluded_pid = excluded_pid;
    if (proc_root != NULL)
        config.proc_root = proc_root;
    if (discovery == NULL || application_discovery_init(discovery, &config) != 0) {
        application_discovery_destroy(discovery);
        return NULL;
    }
    return discovery;
}

static int find_pid(application_discovery_t *discovery, pid_t pid,
                    application_discovery_record_t *result)
{
    for (size_t index = 0; index < application_discovery_count(discovery); index++) {
        application_discovery_record_t record;

        if (!application_discovery_get(discovery, index, &record))
            return 0;
        if (record.pid == pid) {
            if (result != NULL)
                *result = record;
            return 1;
        }
    }
    return 0;
}

static int valid_scan(application_discovery_t *discovery)
{
    size_t count = application_discovery_count(discovery);

    if (count == 0)
        return 0;
    for (size_t index = 0; index < count; index++) {
        application_discovery_record_t record;

        if (!application_discovery_get(discovery, index, &record) ||
            record.pid <= 0 || record.parent_pid < 0 ||
            record.start_time_ticks == 0 || record.state == '\0')
            return 0;
        for (size_t previous = 0; previous < index; previous++) {
            application_discovery_record_t previous_record;

            if (!application_discovery_get(discovery, previous, &previous_record) ||
                previous_record.pid == record.pid)
                return 0;
        }
    }
    return 1;
}

static int write_fixture_stat(const char *root, pid_t pid, const char *name,
                              uint64_t start_time)
{
    char directory_path[512];
    char stat_path[512];
    FILE *file;

    if (snprintf(directory_path, sizeof(directory_path), "%s/%ld", root, (long)pid) >= (int)sizeof(directory_path) ||
        snprintf(stat_path, sizeof(stat_path), "%s/stat", directory_path) >= (int)sizeof(stat_path) ||
        mkdir(directory_path, 0700) != 0)
        return -1;
    file = fopen(stat_path, "w");
    if (file == NULL)
        return -1;
    fprintf(file, "%ld (%s) S 1", (long)pid, name);
    for (unsigned field = 5; field <= 21; field++)
        fprintf(file, " 0");
    fprintf(file, " %llu\n", (unsigned long long)start_time);
    return fclose(file);
}

static int make_fixture_root(char *path, size_t size)
{
    if (snprintf(path, size, "/tmp/awavma-discovery-XXXXXX") >= (int)size)
        return -1;
    return mkdtemp(path) == NULL ? -1 : 0;
}

static void remove_fixture(const char *root, pid_t pid)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%ld/stat", root, (long)pid);
    unlink(path);
    snprintf(path, sizeof(path), "%s/%ld", root, (long)pid);
    rmdir(path);
    rmdir(root);
}

static int child_ready_and_alive(pid_t *child)
{
    int pipe_fds[2];
    char ready;
    pid_t value;
    struct timespec delay = {0, 300000000L};

    if (pipe(pipe_fds) != 0)
        return -1;
    value = fork();
    if (value < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (value == 0) {
        close(pipe_fds[0]);
        if (write(pipe_fds[1], "r", 1) != 1)
            _exit(1);
        close(pipe_fds[1]);
        nanosleep(&delay, NULL);
        _exit(0);
    }
    close(pipe_fds[1]);
    if (read(pipe_fds[0], &ready, 1) != 1) {
        close(pipe_fds[0]);
        waitpid(value, NULL, 0);
        return -1;
    }
    close(pipe_fds[0]);
    *child = value;
    return 0;
}

int main(void)
{
    application_discovery_t *discovery;
    application_discovery_record_t record;
    pid_t child;
    char fixture[128];
    int first_scan;

    discovery = create_discovery(getpid(), NULL);
    report_result("D01", discovery != NULL);
    if (discovery == NULL)
        return EXIT_FAILURE;
    application_discovery_destroy(discovery);

    discovery = create_discovery(0, NULL);
    report_result("D02",
                  discovery != NULL && application_discovery_scan(discovery) == 0 &&
                  find_pid(discovery, getpid(), &record) && record.pid == getpid());
    application_discovery_destroy(discovery);

    discovery = create_discovery(getpid(), NULL);
    child = -1;
    report_result("D03", discovery != NULL && child_ready_and_alive(&child) == 0 &&
                  application_discovery_scan(discovery) == 0 && find_pid(discovery, child, NULL));
    if (child > 0)
        waitpid(child, NULL, 0);
    application_discovery_destroy(discovery);

    discovery = create_discovery(getpid(), NULL);
    child = -1;
    first_scan = discovery != NULL && child_ready_and_alive(&child) == 0 &&
                 application_discovery_scan(discovery) == 0 && find_pid(discovery, child, NULL);
    if (child > 0)
        waitpid(child, NULL, 0);
    report_result("D04", first_scan && application_discovery_scan(discovery) == 0 &&
                  !find_pid(discovery, child, NULL));
    application_discovery_destroy(discovery);

    if (make_fixture_root(fixture, sizeof(fixture)) == 0) {
        char invalid_directory[512];
        char invalid_stat[1024];
        FILE *file;

        snprintf(invalid_directory, sizeof(invalid_directory), "%s/99999", fixture);
        snprintf(invalid_stat, sizeof(invalid_stat), "%s/stat", invalid_directory);
        mkdir(invalid_directory, 0700);
        file = fopen(invalid_stat, "w");
        if (file != NULL) {
            fputs("not a proc stat line\n", file);
            fclose(file);
        }
        discovery = create_discovery(0, fixture);
        report_result("D05", discovery != NULL && application_discovery_scan(discovery) == 0 &&
                      application_discovery_count(discovery) == 0);
        application_discovery_destroy(discovery);
        unlink(invalid_stat);
        rmdir(invalid_directory);
        rmdir(fixture);
    } else {
        report_result("D05", 0);
    }

    if (make_fixture_root(fixture, sizeof(fixture)) == 0 &&
        write_fixture_stat(fixture, 99998, "unreadable-exe", 98765) == 0) {
        int d06_pass;

        discovery = create_discovery(0, fixture);
        d06_pass = discovery != NULL && application_discovery_scan(discovery) == 0 &&
                   find_pid(discovery, 99998, &record) && record.executable_path[0] == '\0';
        report_result("D06", d06_pass);
        application_discovery_destroy(discovery);
        remove_fixture(fixture, 99998);
    } else {
        report_result("D06", 0);
    }

    discovery = create_discovery(0, NULL);
    report_result("D07", discovery != NULL && application_discovery_scan(discovery) == 0 &&
                  find_pid(discovery, getpid(), &record) && record.start_time_ticks > 0 &&
                  record.pid == getpid());
    report_result("D08", discovery != NULL && application_discovery_count(discovery) > 0);
    if (discovery != NULL) {
        int duplicate = 0;

        for (size_t left = 0; left < application_discovery_count(discovery); left++) {
            application_discovery_record_t left_record;

            application_discovery_get(discovery, left, &left_record);
            for (size_t right = left + 1; right < application_discovery_count(discovery); right++) {
                application_discovery_record_t right_record;

                application_discovery_get(discovery, right, &right_record);
                if (left_record.pid == right_record.pid)
                    duplicate = 1;
            }
        }
        report_result("D08-unique-pids", duplicate == 0);
    } else {
        report_result("D08-unique-pids", 0);
    }
    application_discovery_destroy(discovery);

    discovery = create_discovery(getpid(), NULL);
    report_result("D09", discovery != NULL && application_discovery_scan(discovery) == 0 &&
                  !find_pid(discovery, getpid(), NULL));
    application_discovery_destroy(discovery);

    child = fork();
    if (child == 0)
        _exit(0);
    discovery = create_discovery(getpid(), NULL);
    report_result("D10", child > 0 && discovery != NULL && application_discovery_scan(discovery) == 0);
    if (child > 0)
        waitpid(child, NULL, 0);
    application_discovery_destroy(discovery);

    discovery = create_discovery(getpid(), NULL);
    first_scan = discovery != NULL && application_discovery_scan(discovery) == 0 &&
                 valid_scan(discovery);
    report_result("D11", first_scan && application_discovery_scan(discovery) == 0 &&
                  valid_scan(discovery));
    report_result("D12", first_scan);
    application_discovery_destroy(discovery);

    printf("Application discovery tests: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
