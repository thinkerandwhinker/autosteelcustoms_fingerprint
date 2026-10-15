/**
 * SHA_256
 *
 * SHA 256 (FIPS 180-4) implementation
 * created for password hashing
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uint32_t state[8];
        uint64_t bit_len;
        uint8_t buf[64];
        size_t buf_len;
    } sha256_ctx_t;

    void sha256_init(sha256_ctx_t *c);
    void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
    void sha256_final(sha256_ctx_t *c, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif
