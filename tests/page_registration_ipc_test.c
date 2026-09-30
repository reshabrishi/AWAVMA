#include "page_candidate_provider.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    PageCandidateWireMessage rejected = {0};
    page_candidate_provider_t *provider;
    char socket_path[108];

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
    page_candidate_provider_destroy(provider);
    return 0;
}
