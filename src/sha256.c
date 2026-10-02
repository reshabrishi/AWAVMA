#include "sha256.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} sha256_context_t;

static const uint32_t constants[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

static uint32_t rotate_right(uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32U - shift));
}

static void transform(sha256_context_t *context, const unsigned char block[64])
{
    uint32_t words[64], a, b, c, d, e, f, g, h;

    for (size_t index = 0; index < 16; index++)
        words[index] = ((uint32_t)block[index * 4] << 24) | ((uint32_t)block[index * 4 + 1] << 16) |
                       ((uint32_t)block[index * 4 + 2] << 8) | block[index * 4 + 3];
    for (size_t index = 16; index < 64; index++) {
        uint32_t s0 = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3);
        uint32_t s1 = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    a = context->state[0]; b = context->state[1]; c = context->state[2]; d = context->state[3];
    e = context->state[4]; f = context->state[5]; g = context->state[6]; h = context->state[7];
    for (size_t index = 0; index < 64; index++) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + s1 + choose + constants[index] + words[index];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = s0 + majority;
        h = g; g = f; f = e; e = d + temporary1; d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
}

static void update(sha256_context_t *context, const unsigned char *data, size_t length)
{
    context->bits += (uint64_t)length * 8U;
    while (length > 0) {
        size_t copied = 64U - context->used;
        if (copied > length) copied = length;
        memcpy(context->block + context->used, data, copied);
        context->used += copied; data += copied; length -= copied;
        if (context->used == 64U) { transform(context, context->block); context->used = 0; }
    }
}

static void finish(sha256_context_t *context, unsigned char digest[32])
{
    size_t index;

    context->block[context->used++] = 0x80U;
    if (context->used > 56U) {
        while (context->used < 64U) context->block[context->used++] = 0;
        transform(context, context->block); context->used = 0;
    }
    while (context->used < 56U) context->block[context->used++] = 0;
    for (index = 0; index < 8; index++) context->block[56 + index] = (unsigned char)(context->bits >> (56U - index * 8U));
    transform(context, context->block);
    for (index = 0; index < 8; index++) {
        digest[index * 4] = (unsigned char)(context->state[index] >> 24);
        digest[index * 4 + 1] = (unsigned char)(context->state[index] >> 16);
        digest[index * 4 + 2] = (unsigned char)(context->state[index] >> 8);
        digest[index * 4 + 3] = (unsigned char)context->state[index];
    }
}

int sha256_file_hex(FILE *file, char output[65])
{
    sha256_context_t context = {{0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
                                 0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U},0,{0},0};
    unsigned char buffer[4096], digest[32];
    static const char hex[] = "0123456789abcdef";
    size_t read;

    if (file == NULL || output == NULL || fseek(file, 0, SEEK_SET) != 0) return -1;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) != 0) update(&context, buffer, read);
    if (ferror(file)) return -1;
    finish(&context, digest);
    for (size_t index = 0; index < sizeof(digest); index++) {
        output[index * 2] = hex[digest[index] >> 4]; output[index * 2 + 1] = hex[digest[index] & 15U];
    }
    output[64] = '\0';
    return fseek(file, 0, SEEK_SET) == 0 ? 0 : -1;
}
