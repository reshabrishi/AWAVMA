#ifndef RUNTIME_SOCKET_DIRECTORY_H
#define RUNTIME_SOCKET_DIRECTORY_H

#include <stdbool.h>
#include <sys/un.h>

typedef struct {
    char directory[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char page_registration_socket[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char worker_evidence_socket[sizeof(((struct sockaddr_un *)0)->sun_path)];
} runtime_socket_directory_t;

bool runtime_socket_directory_create(runtime_socket_directory_t *directory);
bool runtime_unix_socket_path_valid(const char *path);
void runtime_socket_directory_remove(runtime_socket_directory_t *directory);

#endif
