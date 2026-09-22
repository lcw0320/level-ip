#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_HASH_LEN 32
#define SHA256_HEX_LEN 65

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t block[64];
    size_t blocklen;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t hash[SHA256_HASH_LEN]);

/* 二进制哈希转 64 字符小写十六进制串（含 '\0' 共 65 字节） */
void sha256_hex(const uint8_t hash[SHA256_HASH_LEN], char out[SHA256_HEX_LEN]);

#endif
