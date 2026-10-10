#define _POSIX_C_SOURCE 200809L

#include "runtime_socket_directory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool trusted_user_runtime_directory(const char *path)
{
    struct stat status;
    return lstat(path, &status) == 0 && S_ISDIR(status.st_mode) &&
           status.st_uid == geteuid() && (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool runtime_unix_socket_path_valid(const char *path)
{
    return path != NULL && path[0] != '\0' &&
           strlen(path) < sizeof(((struct sockaddr_un *)0)->sun_path);
}

bool runtime_socket_directory_create(runtime_socket_directory_t *directory)
{
    char user_base[64];
    const char *base = "/tmp";
    struct stat status;

    if (directory == NULL)
        return false;
    memset(directory, 0, sizeof(*directory));
    if (snprintf(user_base, sizeof(user_base), "/run/user/%lu", (unsigned long)geteuid()) > 0 &&
        trusted_user_runtime_directory(user_base))
        base = user_base;
    if (snprintf(directory->directory, sizeof(directory->directory), "%s/awavma-XXXXXX", base) >=
            (int)sizeof(directory->directory) || mkdtemp(directory->directory) == NULL ||
        chmod(directory->directory, S_IRWXU) != 0 || lstat(directory->directory, &status) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0777) != S_IRWXU ||
        snprintf(directory->page_registration_socket, sizeof(directory->page_registration_socket),
                 "%s/page-registration.sock", directory->directory) >=
            (int)sizeof(directory->page_registration_socket) ||
        snprintf(directory->worker_evidence_socket, sizeof(directory->worker_evidence_socket),
                 "%s/worker-evidence.sock", directory->directory) >=
            (int)sizeof(directory->worker_evidence_socket)) {
        runtime_socket_directory_remove(directory);
        return false;
    }
    if (!runtime_unix_socket_path_valid(directory->page_registration_socket) ||
        !runtime_unix_socket_path_valid(directory->worker_evidence_socket)) {
        runtime_socket_directory_remove(directory);
        return false;
    }
    return true;
}

void runtime_socket_directory_remove(runtime_socket_directory_t *directory)
{
    if (directory == NULL)
        return;
    if (directory->directory[0] != '\0')
        (void)rmdir(directory->directory);
    memset(directory, 0, sizeof(*directory));
}
