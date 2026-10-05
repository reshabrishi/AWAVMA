#ifndef AWAVMA_PAGE_CANDIDATE_PROVIDER_H
#define AWAVMA_PAGE_CANDIDATE_PROVIDER_H

#include "migration_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A migration/checkpoint attempt is bounded independently of region ownership. */
#define PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST 4096U
/* Compact registrations may describe large workloads without per-page metadata. */
#define PAGE_CANDIDATE_MAX_REGISTERED_REGION_BYTES (1024ULL * 1024ULL * 1024ULL * 1024ULL)
#define PAGE_CANDIDATE_MAX_REGISTRATIONS 64U
#define PAGE_CANDIDATE_APP_ID_MAX 128U
#define PAGE_CANDIDATE_PROVENANCE_MAX 96U
#define PAGE_CANDIDATE_WIRE_VERSION 1U
#define PAGE_CANDIDATE_RESPONSE_VERSION 1U

typedef struct page_candidate_provider page_candidate_provider_t;

typedef struct {
    const char *app_id;
    pid_t pid;
    uint64_t start_time_ticks;
    void *region_start;
    size_t region_length;
    size_t page_size;
    const char *provenance;
} PageCandidateRegistration;

/* Fixed-size local IPC message. Addresses are never written to persistent state. */
typedef struct {
    uint32_t version;
    uint32_t operation;
    int64_t pid;
    uint64_t start_time_ticks;
    uint64_t region_start;
    uint64_t region_length;
    uint64_t page_size;
    uint64_t client_generation;
    char app_id[PAGE_CANDIDATE_APP_ID_MAX];
    char provenance[PAGE_CANDIDATE_PROVENANCE_MAX];
} PageCandidateWireMessage;

enum {
    PAGE_CANDIDATE_WIRE_REGISTER = 1U,
    PAGE_CANDIDATE_WIRE_UNREGISTER = 2U
};

typedef enum {
    PAGE_CANDIDATE_STATUS_ACCEPTED,
    PAGE_CANDIDATE_STATUS_REJECTED,
    PAGE_CANDIDATE_STATUS_TIMEOUT,
    PAGE_CANDIDATE_STATUS_PROTOCOL,
    PAGE_CANDIDATE_STATUS_IO
} PageCandidateClientStatus;

typedef enum {
    PAGE_CANDIDATE_REASON_ACCEPTED,
    PAGE_CANDIDATE_REASON_MALFORMED,
    PAGE_CANDIDATE_REASON_UNSUPPORTED_VERSION,
    PAGE_CANDIDATE_REASON_PEER_CREDENTIAL,
    PAGE_CANDIDATE_REASON_IDENTITY,
    PAGE_CANDIDATE_REASON_INVALID_REGION,
    PAGE_CANDIDATE_REASON_STALE_GENERATION,
    PAGE_CANDIDATE_REASON_CAPACITY
} PageCandidateResponseReason;

/* Fixed-size response; it intentionally contains no address information. */
typedef struct {
    uint32_t version;
    uint32_t operation;
    uint64_t client_generation;
    uint32_t accepted;
    uint32_t reason;
} PageCandidateWireResponse;

typedef struct {
    bool accepted;
    uint64_t generation;
    size_t registered_bytes;
    size_t registered_pages;
    size_t candidate_pages_per_request;
} PageCandidateRegistrationStatus;

page_candidate_provider_t *page_candidate_provider_create(void);
void page_candidate_provider_destroy(page_candidate_provider_t *provider);
/* The runtime owns this socket and calls poll from its foreground cycle. */
bool page_candidate_provider_start(page_candidate_provider_t *provider, const char *socket_path,
                                   uint64_t registration_ttl_ms);
void page_candidate_provider_stop(page_candidate_provider_t *provider);
bool page_candidate_provider_poll(page_candidate_provider_t *provider);
bool page_candidate_provider_send(const char *socket_path, const PageCandidateWireMessage *message);
PageCandidateClientStatus page_candidate_provider_send_wait(
    const char *socket_path, const PageCandidateWireMessage *message, uint64_t timeout_ms,
    PageCandidateResponseReason *reason);
bool page_candidate_provider_registration_status(page_candidate_provider_t *provider,
                                                 const char *app_id, pid_t pid,
                                                 uint64_t start_time_ticks,
                                                 PageCandidateRegistrationStatus *status);
/* Must be called by the workload process that owns the allocation. */
bool page_candidate_provider_register_owned_region(page_candidate_provider_t *provider,
                                                   const PageCandidateRegistration *registration);
bool page_candidate_provider_fill_request(page_candidate_provider_t *provider, const char *app_id,
                                          pid_t pid, uint64_t start_time_ticks,
                                          int destination_node, MigrationRequest *request);
void page_candidate_provider_release_request(MigrationRequest *request);

#endif
