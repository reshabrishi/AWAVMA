#define _POSIX_C_SOURCE 200809L

#include "page_candidate_provider.h"
#include "runtime_migration_metadata.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    PageCandidateRegistration registration;
    char app_id[PAGE_CANDIDATE_APP_ID_MAX];
    char provenance[PAGE_CANDIDATE_PROVENANCE_MAX];
    uint64_t generation;
    uint64_t expires_at_ms;
    bool used;
} registration_entry_t;

struct page_candidate_provider {
    registration_entry_t entries[PAGE_CANDIDATE_MAX_REGISTRATIONS];
    int socket_fd;
    uint64_t ttl_ms;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool safe_text(const char *value, size_t capacity)
{
    return value != NULL && value[0] != '\0' && strnlen(value, capacity) < capacity &&
           strchr(value, ',') == NULL && strchr(value, '\n') == NULL;
}

static bool proc_maps_contains(pid_t pid, uintptr_t start, size_t length)
{
    char path[64], line[512];
    FILE *file;
    uintptr_t end;

    if (length == 0 || start > UINTPTR_MAX - length ||
        snprintf(path, sizeof(path), "/proc/%ld/maps", (long)pid) >= (int)sizeof(path))
        return false;
    end = start + length;
    file = fopen(path, "r");
    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long map_start, map_end;
        char permissions[5] = {0};

        if (sscanf(line, "%llx-%llx %4s", &map_start, &map_end, permissions) == 3 &&
            start >= (uintptr_t)map_start && end <= (uintptr_t)map_end &&
            permissions[0] == 'r' && permissions[1] == 'w') {
            fclose(file);
            return true;
        }
    }
    fclose(file);
    return false;
}

static bool valid_registration(const PageCandidateRegistration *registration)
{
    RuntimeMigrationMetadata metadata;

    if (registration == NULL || !safe_text(registration->app_id, PAGE_CANDIDATE_APP_ID_MAX) ||
        !safe_text(registration->provenance, PAGE_CANDIDATE_PROVENANCE_MAX) || registration->pid <= 0 ||
        registration->start_time_ticks == 0 || registration->region_start == NULL ||
        registration->page_size == 0 || registration->region_length == 0 ||
        registration->region_length % registration->page_size != 0 ||
        (uintptr_t)registration->region_start % registration->page_size != 0)
        return false;
    return registration->region_length <= PAGE_CANDIDATE_MAX_REGISTERED_REGION_BYTES &&
            runtime_get_migration_metadata(registration->pid, registration->start_time_ticks, &metadata) &&
           metadata.identity_match && proc_maps_contains(registration->pid,
                                                         (uintptr_t)registration->region_start,
                                                         registration->region_length);
}

static bool valid_unregistration(const PageCandidateRegistration *registration)
{
    RuntimeMigrationMetadata metadata;

    return registration != NULL && registration->pid > 0 && registration->start_time_ticks != 0 &&
           safe_text(registration->app_id, PAGE_CANDIDATE_APP_ID_MAX) &&
           safe_text(registration->provenance, PAGE_CANDIDATE_PROVENANCE_MAX) &&
           runtime_get_migration_metadata(registration->pid, registration->start_time_ticks, &metadata) &&
           metadata.identity_match;
}

static void expire_entries(page_candidate_provider_t *provider)
{
    uint64_t now = monotonic_ms();
    for (size_t index = 0; index < PAGE_CANDIDATE_MAX_REGISTRATIONS; index++)
        if (provider->entries[index].used && provider->entries[index].expires_at_ms <= now)
            provider->entries[index].used = false;
}

static bool store_registration(page_candidate_provider_t *provider,
                               const PageCandidateRegistration *registration,
                               uint64_t client_generation)
{
    registration_entry_t *entry = NULL;
    size_t free_index = PAGE_CANDIDATE_MAX_REGISTRATIONS;

    expire_entries(provider);
    for (size_t index = 0; index < PAGE_CANDIDATE_MAX_REGISTRATIONS; index++) {
        registration_entry_t *candidate = &provider->entries[index];
        if (candidate->used && candidate->registration.pid == registration->pid &&
            candidate->registration.start_time_ticks == registration->start_time_ticks &&
            strcmp(candidate->app_id, registration->app_id) == 0) {
            entry = candidate;
            break;
        }
        if (!candidate->used && free_index == PAGE_CANDIDATE_MAX_REGISTRATIONS)
            free_index = index;
    }
    if (entry != NULL && client_generation <= entry->generation)
        return false;
    if (entry == NULL && free_index == PAGE_CANDIDATE_MAX_REGISTRATIONS)
        return false;
    if (entry == NULL)
        entry = &provider->entries[free_index];
    memset(entry, 0, sizeof(*entry));
    entry->registration = *registration;
    snprintf(entry->app_id, sizeof(entry->app_id), "%s", registration->app_id);
    snprintf(entry->provenance, sizeof(entry->provenance), "%s", registration->provenance);
    entry->registration.app_id = entry->app_id;
    entry->registration.provenance = entry->provenance;
    entry->generation = client_generation == 0 ? 1 : client_generation;
    entry->expires_at_ms = monotonic_ms() + provider->ttl_ms;
    entry->used = true;
    return true;
}

static PageCandidateResponseReason registration_reason(page_candidate_provider_t *provider,
                                                        const PageCandidateRegistration *registration,
                                                        uint64_t generation)
{
    if (!valid_registration(registration))
        return PAGE_CANDIDATE_REASON_INVALID_REGION;
    for (size_t index = 0; index < PAGE_CANDIDATE_MAX_REGISTRATIONS; index++) {
        registration_entry_t *entry = &provider->entries[index];
        if (entry->used && entry->registration.pid == registration->pid &&
            entry->registration.start_time_ticks == registration->start_time_ticks &&
            strcmp(entry->app_id, registration->app_id) == 0 && generation <= entry->generation)
            return PAGE_CANDIDATE_REASON_STALE_GENERATION;
    }
    return PAGE_CANDIDATE_REASON_CAPACITY;
}

static void send_response(int client, const PageCandidateWireMessage *message, bool accepted,
                          PageCandidateResponseReason reason)
{
    PageCandidateWireResponse response = {
        .version = PAGE_CANDIDATE_RESPONSE_VERSION,
        .operation = message == NULL ? 0 : message->operation,
        .client_generation = message == NULL ? 0 : message->client_generation,
        .accepted = accepted ? 1U : 0U,
        .reason = reason
    };
    (void)send(client, &response, sizeof(response), MSG_NOSIGNAL);
}

page_candidate_provider_t *page_candidate_provider_create(void)
{
    page_candidate_provider_t *provider = calloc(1, sizeof(*provider));
    if (provider != NULL) {
        provider->socket_fd = -1;
        provider->ttl_ms = 30000;
    }
    return provider;
}

void page_candidate_provider_stop(page_candidate_provider_t *provider)
{
    if (provider == NULL)
        return;
    if (provider->socket_fd >= 0)
        close(provider->socket_fd);
    provider->socket_fd = -1;
    if (provider->socket_path[0] != '\0')
        unlink(provider->socket_path);
    provider->socket_path[0] = '\0';
}

void page_candidate_provider_destroy(page_candidate_provider_t *provider)
{
    if (provider != NULL)
        page_candidate_provider_stop(provider);
    free(provider);
}

bool page_candidate_provider_start(page_candidate_provider_t *provider, const char *socket_path,
                                   uint64_t registration_ttl_ms)
{
    struct sockaddr_un address = {0};
    int flags;

    if (provider == NULL || socket_path == NULL || socket_path[0] == '\0' ||
        strlen(socket_path) >= sizeof(address.sun_path) || registration_ttl_ms == 0)
        return false;
    page_candidate_provider_stop(provider);
    provider->socket_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (provider->socket_fd < 0)
        return false;
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    unlink(socket_path);
    if (bind(provider->socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(socket_path, S_IRUSR | S_IWUSR) != 0 || listen(provider->socket_fd, 16) != 0 ||
        (flags = fcntl(provider->socket_fd, F_GETFL, 0)) < 0 ||
        fcntl(provider->socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        page_candidate_provider_stop(provider);
        return false;
    }
    provider->ttl_ms = registration_ttl_ms;
    snprintf(provider->socket_path, sizeof(provider->socket_path), "%s", socket_path);
    return true;
}

bool page_candidate_provider_poll(page_candidate_provider_t *provider)
{
    bool accepted = false;

    if (provider == NULL || provider->socket_fd < 0)
        return false;
    expire_entries(provider);
    for (;;) {
        int client = accept(provider->socket_fd, NULL, NULL);
        struct ucred credential;
        socklen_t credential_size = sizeof(credential);
        PageCandidateWireMessage message = {0};
        ssize_t received;

        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        received = recv(client, &message, sizeof(message), MSG_TRUNC);
        bool well_formed = received == (ssize_t)sizeof(message) &&
            message.version == PAGE_CANDIDATE_WIRE_VERSION && message.region_start <= UINTPTR_MAX &&
            message.region_length <= SIZE_MAX && message.page_size <= SIZE_MAX &&
            safe_text(message.app_id, sizeof(message.app_id)) &&
            safe_text(message.provenance, sizeof(message.provenance)) &&
            (message.operation == PAGE_CANDIDATE_WIRE_REGISTER ||
             message.operation == PAGE_CANDIDATE_WIRE_UNREGISTER);
        PageCandidateResponseReason reason = message.version != PAGE_CANDIDATE_WIRE_VERSION ?
            PAGE_CANDIDATE_REASON_UNSUPPORTED_VERSION : PAGE_CANDIDATE_REASON_MALFORMED;
        bool stored = false;
        if (well_formed && getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credential, &credential_size) == 0 &&
            credential.uid == geteuid() && message.pid == credential.pid && message.pid > 0) {
            PageCandidateRegistration registration = {
                .app_id = message.app_id, .pid = (pid_t)message.pid,
                .start_time_ticks = message.start_time_ticks,
                .region_start = (void *)(uintptr_t)message.region_start,
                .region_length = (size_t)message.region_length, .page_size = (size_t)message.page_size,
                .provenance = message.provenance
            };
            if (message.operation == PAGE_CANDIDATE_WIRE_REGISTER) {
                stored = store_registration(provider, &registration, message.client_generation);
                reason = stored ? PAGE_CANDIDATE_REASON_ACCEPTED :
                    registration_reason(provider, &registration, message.client_generation);
                accepted |= stored;
            }
            else if (message.operation == PAGE_CANDIDATE_WIRE_UNREGISTER) {
                for (size_t index = 0; index < PAGE_CANDIDATE_MAX_REGISTRATIONS; index++)
                    if (valid_unregistration(&registration) && provider->entries[index].used &&
                        provider->entries[index].registration.pid == registration.pid &&
                        provider->entries[index].registration.start_time_ticks == registration.start_time_ticks &&
                        strcmp(provider->entries[index].app_id, registration.app_id) == 0 &&
                        strcmp(provider->entries[index].provenance, registration.provenance) == 0) {
                        provider->entries[index].used = false;
                        accepted = true;
                        stored = true;
                        reason = PAGE_CANDIDATE_REASON_ACCEPTED;
                    }
            }
            if (!stored && message.operation == PAGE_CANDIDATE_WIRE_UNREGISTER)
                reason = PAGE_CANDIDATE_REASON_IDENTITY;
        } else if (well_formed)
            reason = PAGE_CANDIDATE_REASON_PEER_CREDENTIAL;
        send_response(client, &message, stored, reason);
        close(client);
    }
    return accepted;
}

PageCandidateClientStatus page_candidate_provider_send_wait(
    const char *socket_path, const PageCandidateWireMessage *message, uint64_t timeout_ms,
    PageCandidateResponseReason *reason)
{
    struct sockaddr_un address = {0};
    struct pollfd wait_fd;
    PageCandidateWireResponse response = {0};
    int socket_fd;
    ssize_t received;

    if (reason != NULL) *reason = PAGE_CANDIDATE_REASON_MALFORMED;
    if (socket_path == NULL || message == NULL || timeout_ms == 0 ||
        strlen(socket_path) >= sizeof(address.sun_path))
        return PAGE_CANDIDATE_STATUS_PROTOCOL;
    socket_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (socket_fd < 0) return PAGE_CANDIDATE_STATUS_IO;
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (connect(socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        send(socket_fd, message, sizeof(*message), 0) != (ssize_t)sizeof(*message)) {
        close(socket_fd); return PAGE_CANDIDATE_STATUS_IO;
    }
    wait_fd.fd = socket_fd; wait_fd.events = POLLIN; wait_fd.revents = 0;
    int poll_result = poll(&wait_fd, 1, timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms);
    if (poll_result == 0) {
        close(socket_fd); return PAGE_CANDIDATE_STATUS_TIMEOUT;
    }
    if (poll_result < 0) {
        close(socket_fd); return PAGE_CANDIDATE_STATUS_IO;
    }
    received = recv(socket_fd, &response, sizeof(response), MSG_TRUNC);
    close(socket_fd);
    if (received != (ssize_t)sizeof(response) || response.version != PAGE_CANDIDATE_RESPONSE_VERSION ||
        response.operation != message->operation || response.client_generation != message->client_generation)
        return PAGE_CANDIDATE_STATUS_PROTOCOL;
    if (reason != NULL) *reason = (PageCandidateResponseReason)response.reason;
    return response.accepted == 1U && response.reason == PAGE_CANDIDATE_REASON_ACCEPTED ?
        PAGE_CANDIDATE_STATUS_ACCEPTED : PAGE_CANDIDATE_STATUS_REJECTED;
}

bool page_candidate_provider_registration_status(page_candidate_provider_t *provider,
                                                 const char *app_id, pid_t pid,
                                                 uint64_t start_time_ticks,
                                                 PageCandidateRegistrationStatus *status)
{
    if (status == NULL) return false;
    memset(status, 0, sizeof(*status));
    if (provider == NULL || !safe_text(app_id, PAGE_CANDIDATE_APP_ID_MAX)) return false;
    expire_entries(provider);
    for (size_t index = 0; index < PAGE_CANDIDATE_MAX_REGISTRATIONS; index++) {
        registration_entry_t *entry = &provider->entries[index];
        if (entry->used && entry->registration.pid == pid &&
            entry->registration.start_time_ticks == start_time_ticks && strcmp(entry->app_id, app_id) == 0) {
            status->accepted = true;
            status->generation = entry->generation;
            status->registered_bytes = entry->registration.region_length;
            status->registered_pages = entry->registration.region_length / entry->registration.page_size;
            status->candidate_pages_per_request = status->registered_pages <
                PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST ? status->registered_pages :
                PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST;
            return true;
        }
    }
    return false;
}

bool page_candidate_placement_accumulate(PageCandidatePlacementEvidence *evidence,
                                         const int *status, size_t count)
{
    if (evidence == NULL || status == NULL || count == 0) return false;
    for (size_t index = 0; index < count; index++) {
        if (status[index] < 0 || status[index] >= (int)PAGE_CANDIDATE_PLACEMENT_MAX_NODES) {
            evidence->unknown_pages++;
            continue;
        }
        evidence->queryable_pages++;
        if (++evidence->node_counts[status[index]] > evidence->dominant_pages) {
            evidence->dominant_pages = evidence->node_counts[status[index]];
            evidence->dominant_node = status[index];
        }
    }
    return true;
}

bool page_candidate_provider_placement_evidence(page_candidate_provider_t *provider,
                                                 const char *app_id, pid_t pid,
                                                 uint64_t start_time_ticks,
                                                 uint64_t registration_generation,
                                                 PageCandidatePlacementEvidence *evidence)
{
    registration_entry_t *entry = NULL;
    RuntimeMigrationMetadata metadata;
    size_t total;

    if (evidence == NULL) return false;
    memset(evidence, 0, sizeof(*evidence));
    evidence->dominant_node = -1;
    if (provider == NULL || !safe_text(app_id, PAGE_CANDIDATE_APP_ID_MAX) || pid <= 0 ||
        start_time_ticks == 0 || registration_generation == 0 ||
        !runtime_get_migration_metadata(pid, start_time_ticks, &metadata) ||
        !metadata.identity_match) return false;
    expire_entries(provider);
    for (size_t index = 0; index < PAGE_CANDIDATE_MAX_REGISTRATIONS; index++)
        if (provider->entries[index].used && provider->entries[index].registration.pid == pid &&
            provider->entries[index].registration.start_time_ticks == start_time_ticks &&
            strcmp(provider->entries[index].app_id, app_id) == 0) { entry = &provider->entries[index]; break; }
    if (entry == NULL || entry->generation != registration_generation ||
        !proc_maps_contains(pid, (uintptr_t)entry->registration.region_start,
                                             entry->registration.region_length) ||
        entry->registration.page_size == 0) return false;
    total = entry->registration.region_length / entry->registration.page_size;
    evidence->identity_match = true; evidence->registration_generation = entry->generation;
    evidence->total_pages = total;
    for (size_t base = 0; base < total; base += PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST) {
        void *pages[PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST];
        int status[PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST];
        size_t count = total - base < PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST ? total - base : PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST;
        for (size_t index = 0; index < count; index++) status[index] = -EIO;
        for (size_t index = 0; index < count; index++) pages[index] =
            (char *)entry->registration.region_start + (base + index) * entry->registration.page_size;
#ifdef SYS_move_pages
        if (syscall(SYS_move_pages, pid, count, pages, NULL, status, 0) < 0) return false;
#else
        return false;
#endif
        if (!page_candidate_placement_accumulate(evidence, status, count)) return false;
    }
    return true;
}

bool page_candidate_provider_send(const char *socket_path, const PageCandidateWireMessage *message)
{
    struct sockaddr_un address = {0};
    int socket_fd;
    bool sent;
    if (socket_path == NULL || message == NULL || strlen(socket_path) >= sizeof(address.sun_path))
        return false;
    socket_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (socket_fd < 0)
        return false;
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    sent = connect(socket_fd, (const struct sockaddr *)&address, sizeof(address)) == 0 &&
           send(socket_fd, message, sizeof(*message), 0) == (ssize_t)sizeof(*message);
    close(socket_fd);
    return sent;
}

bool page_candidate_provider_register_owned_region(page_candidate_provider_t *provider,
                                                    const PageCandidateRegistration *registration)
{
    return provider != NULL && registration != NULL && registration->pid == getpid() &&
           valid_registration(registration) && store_registration(provider, registration, 1);
}

bool page_candidate_provider_fill_request(page_candidate_provider_t *provider, const char *app_id,
                                           pid_t pid, uint64_t start_time_ticks,
                                           int destination_node, MigrationRequest *request)
{
    RuntimeMigrationMetadata metadata;
    registration_entry_t *entry = NULL;
    size_t count;
    if (provider == NULL || request == NULL || !safe_text(app_id, PAGE_CANDIDATE_APP_ID_MAX) ||
        destination_node < -1 || !runtime_get_migration_metadata(pid, start_time_ticks, &metadata) ||
        !metadata.identity_match)
        return false;
    expire_entries(provider);
    for (size_t index = 0; index < PAGE_CANDIDATE_MAX_REGISTRATIONS; index++)
        if (provider->entries[index].used && provider->entries[index].registration.pid == pid &&
            provider->entries[index].registration.start_time_ticks == start_time_ticks &&
            strcmp(provider->entries[index].app_id, app_id) == 0) {
            entry = &provider->entries[index];
            break;
        }
    if (entry == NULL || !proc_maps_contains(pid, (uintptr_t)entry->registration.region_start,
                                             entry->registration.region_length))
        return false;
    count = entry->registration.region_length / entry->registration.page_size;
    if (count > PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST)
        count = PAGE_CANDIDATE_MAX_PAGES_PER_REQUEST;
    request->pages = calloc(count, sizeof(*request->pages));
    if (request->pages == NULL)
        return false;
    for (size_t index = 0; index < count; index++)
        request->pages[index] = (void *)((uintptr_t)entry->registration.region_start +
                                         index * entry->registration.page_size);
    request->page_count = count;
    request->page_metadata_available = true;
    request->page_addresses_authoritative = true;
    request->memory_region_verified = true;
    if (destination_node >= 0)
        request->destination_numa_node = destination_node;
    return true;
}

void page_candidate_provider_release_request(MigrationRequest *request)
{
    if (request != NULL) {
        free(request->pages);
        request->pages = NULL;
        request->page_count = 0;
        request->page_metadata_available = false;
        request->page_addresses_authoritative = false;
        request->memory_region_verified = false;
    }
}
