/* SHA-256 实现（FIPS 180-4），无第三方依赖 */

#include "sha256.h"
#include <string.h>

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(sha256_ctx *ctx, const uint8_t block[64])
{
    uint32_t w[64] = {0};
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    uint32_t d = 0;
    uint32_t e = 0;
    uint32_t f = 0;
    uint32_t g = 0;
    uint32_t h = 0;
    uint32_t t1 = 0;
    uint32_t t2 = 0;
    int i = 0;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        t1 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        t2 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + t1 + w[i - 7] + t2;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->blocklen = 0;
}

void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t fill = 0;

    ctx->bitlen += (uint64_t)len * 8;
    while (len > 0) {
        fill = 64 - ctx->blocklen;
        if (fill > len) {
            fill = len;
        }
        memcpy(ctx->block + ctx->blocklen, data, fill);
        ctx->blocklen += fill;
        data += fill;
        len -= fill;
        if (ctx->blocklen == 64) {
            sha256_transform(ctx, ctx->block);
            ctx->blocklen = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t hash[SHA256_HASH_LEN])
{
    uint8_t pad = 0x80;
    uint8_t zeros[64] = {0};
    uint8_t lenbuf[8] = {0};
    size_t padlen = 0;
    int i = 0;
    uint64_t bitlen = ctx->bitlen;

    sha256_update(ctx, &pad, 1);
    padlen = (ctx->blocklen > 56) ? (120 - ctx->blocklen) : (56 - ctx->blocklen);
    if (padlen > 0) {
        sha256_update(ctx, zeros, padlen);
    }
    for (i = 0; i < 8; i++) {
        lenbuf[i] = (uint8_t)(bitlen >> (56 - i * 8));
    }
    sha256_update(ctx, lenbuf, 8);

    for (i = 0; i < 8; i++) {
        hash[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256_hex(const uint8_t hash[SHA256_HASH_LEN], char out[SHA256_HEX_LEN])
{
    static const char hexchars[] = "0123456789abcdef";
    int i = 0;

    for (i = 0; i < SHA256_HASH_LEN; i++) {
        out[i * 2] = hexchars[hash[i] >> 4];
        out[i * 2 + 1] = hexchars[hash[i] & 0x0f];
    }
    out[SHA256_HASH_LEN * 2] = '\0';
}
