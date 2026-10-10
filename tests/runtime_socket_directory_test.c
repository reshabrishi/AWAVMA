#define _POSIX_C_SOURCE 200809L
#include "page_candidate_provider.h"
#include "runtime_socket_directory.h"
#include "worker_evidence_provider.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    runtime_socket_directory_t first, second;
    page_candidate_provider_t *pages = page_candidate_provider_create();
    worker_evidence_provider_t *workers = worker_evidence_provider_create();
    struct stat status;
    char removed[sizeof(first.directory)];
    int fd;

    assert(pages != NULL && workers != NULL);
    assert(runtime_socket_directory_create(&first));
    assert(runtime_socket_directory_create(&second));
    assert(strcmp(first.directory, second.directory) != 0);
    assert(runtime_unix_socket_path_valid(first.page_registration_socket));
    assert(runtime_unix_socket_path_valid(first.worker_evidence_socket));
    assert(!runtime_unix_socket_path_valid(""));
    assert(lstat(first.directory, &status) == 0 && status.st_uid == geteuid() &&
           (status.st_mode & 0777) == 0700);
    assert(page_candidate_provider_start(pages, first.page_registration_socket, 1000));
    assert(worker_evidence_provider_start(workers, first.worker_evidence_socket));
    page_candidate_provider_stop(pages);
    worker_evidence_provider_stop(workers);
    fd = open(first.page_registration_socket, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(!page_candidate_provider_start(pages, first.page_registration_socket, 1000));
    assert(unlink(first.page_registration_socket) == 0);
    assert(symlink("missing-target", first.worker_evidence_socket) == 0);
    assert(!worker_evidence_provider_start(workers, first.worker_evidence_socket));
    assert(unlink(first.worker_evidence_socket) == 0);
    snprintf(removed, sizeof(removed), "%s", first.directory);
    runtime_socket_directory_remove(&first);
    runtime_socket_directory_remove(&second);
    assert(lstat(removed, &status) != 0);
    page_candidate_provider_destroy(pages);
    worker_evidence_provider_destroy(workers);
    puts("runtime_socket_directory_test: PASS");
    return 0;
}
