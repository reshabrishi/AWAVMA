#include "page_candidate_provider.h"
#include "runtime_migration_metadata.h"

#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    PageCandidateWireMessage rejected = {0};
    page_candidate_provider_t *provider;
    char socket_path[108];
    pid_t child;
    int child_status;
    void *large_region;
    void *small_region;
    void *window_region;
    uint64_t ticks;
    long page_size;
    size_t large_bytes;
    PageCandidateRegistration large_registration;
    PageCandidateRegistration small_registration;
    PageCandidateRegistration window_registration;
    PageCandidateRegistrationStatus large_status;
    PageCandidateRegistrationStatus expired_status;
    MigrationRequest request = {0};

    assert(sizeof(PageCandidateWireMessage) == 280U);
    assert(snprintf(socket_path, sizeof(socket_path), "/tmp/awavma-page-ipc-%ld.sock", (long)getpid()) > 0);
    provider = page_candidate_provider_create();
    assert(provider != NULL);
    assert(page_candidate_provider_start(provider, socket_path, 1000));
    rejected.version = PAGE_CANDIDATE_WIRE_VERSION - 1U;
    rejected.operation = PAGE_CANDIDATE_WIRE_REGISTER;
    rejected.pid = getpid();
    assert(page_candidate_provider_send(socket_path, &rejected));
    assert(!page_candidate_provider_poll(provider));
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        PageCandidateWireMessage accepted = {0};
        PageCandidateResponseReason reason;
        uint64_t generation = 0;
        long page_size = sysconf(_SC_PAGESIZE);
        size_t length = ((size_t)PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST + 1U) * (size_t)page_size;
        void *region = mmap(NULL, length, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint64_t ticks = 0;

        if (region == MAP_FAILED || !runtime_read_start_time_ticks(getpid(), &ticks))
            _exit(2);
        accepted.version = PAGE_CANDIDATE_WIRE_VERSION;
        accepted.operation = PAGE_CANDIDATE_WIRE_REGISTER;
        accepted.pid = getpid();
        accepted.start_time_ticks = ticks;
        accepted.region_start = (uintptr_t)region;
        accepted.region_length = length;
        accepted.page_size = (uint64_t)page_size;
        accepted.client_generation = 7;
        snprintf(accepted.app_id, sizeof(accepted.app_id), "APP_%ld_%llu", (long)getpid(),
                 (unsigned long long)ticks);
        snprintf(accepted.provenance, sizeof(accepted.provenance), "ipc-test-owned");
        _exit(page_candidate_provider_send_wait(socket_path, &accepted, 2000, &reason, &generation) ==
                      PAGE_CANDIDATE_STATUS_ACCEPTED && reason == PAGE_CANDIDATE_REASON_ACCEPTED &&
                      generation == accepted.client_generation ? 0 : 3);
    }
    for (unsigned tries = 0; tries < 20; tries++) {
        if (page_candidate_provider_poll(provider))
            break;
        usleep(10000);
    }
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    printf("C1G01_ACK_GENERATION_RETURNED: PASS\n");
    page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    large_bytes = 1024U * 1024U * 1024U;
    assert(large_bytes % (size_t)page_size == 0);
    large_region = mmap(NULL, large_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(large_region != MAP_FAILED);
    assert(runtime_read_start_time_ticks(getpid(), &ticks));
    large_registration = (PageCandidateRegistration){
        .app_id = "large-owned-region", .pid = getpid(), .start_time_ticks = ticks,
        .region_start = large_region, .region_length = large_bytes, .page_size = (size_t)page_size,
        .provenance = "ipc-test-owned"
    };
    assert(page_candidate_provider_register_owned_region(provider, &large_registration));
    assert(page_candidate_provider_registration_status(provider, large_registration.app_id, getpid(), ticks,
                                                         &large_status));
    assert(large_status.accepted && large_status.generation == 1U);
    assert(large_status.registered_bytes == large_bytes);
    assert(large_status.registered_pages == large_bytes / (size_t)page_size);
    assert(large_status.candidate_pages_per_request == PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST);
    assert(page_candidate_provider_fill_request(provider, large_registration.app_id, getpid(), ticks, -1,
                                                &request));
    assert(request.page_count == PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST);
    page_candidate_provider_release_request(&request);
    small_region = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(small_region != MAP_FAILED);
    small_registration = large_registration;
    small_registration.app_id = "small-owned-region";
    small_registration.region_start = small_region;
    small_registration.region_length = (size_t)page_size;
    assert(page_candidate_provider_register_owned_region(provider, &small_registration));
    assert(page_candidate_provider_fill_request(provider, small_registration.app_id, getpid(), ticks, -1,
                                                &request));
    assert(request.page_count == 1);
    page_candidate_provider_release_request(&request);
    window_region = mmap(NULL, (size_t)PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST * (size_t)page_size,
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(window_region != MAP_FAILED);
    window_registration = large_registration;
    window_registration.app_id = "window-owned-region";
    window_registration.region_start = window_region;
    window_registration.region_length = (size_t)PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST * (size_t)page_size;
    assert(page_candidate_provider_register_owned_region(provider, &window_registration));
    assert(page_candidate_provider_fill_request(provider, window_registration.app_id, getpid(), ticks, -1,
                                                &request));
    assert(request.page_count == PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST);
    page_candidate_provider_release_request(&request);
    large_registration.region_length = PAGE_CANDIDATE_MAX_REGISTERED_REGION_BYTES + (size_t)page_size;
    assert(!page_candidate_provider_register_owned_region(provider, &large_registration));
    usleep(1100000);
    assert(!page_candidate_provider_registration_status(provider, large_registration.app_id, getpid(), ticks,
                                                         &expired_status));
    large_registration.region_length = (size_t)page_size;
    large_registration.region_start = (void *)(UINTPTR_MAX - (uintptr_t)page_size + 1U);
    assert(!page_candidate_provider_register_owned_region(provider, &large_registration));
    munmap(window_region, window_registration.region_length);
    munmap(small_region, small_registration.region_length);
    munmap(large_region, large_bytes);
    page_candidate_provider_destroy(provider);
    return 0;
}
