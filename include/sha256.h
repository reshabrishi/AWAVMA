#ifndef AWAVMA_SHA256_H
#define AWAVMA_SHA256_H

#include <stddef.h>
#include <stdio.h>

/* Writes a lowercase SHA-256 digest for the exact bytes in an already-open file. */
int sha256_file_hex(FILE *file, char output[65]);

#endif
