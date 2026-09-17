#ifndef KOS_CRYPTO_H
#define KOS_CRYPTO_H

#include <stdbool.h>
#include <stdint.h>

struct kos_sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t buffer[64];
    uint64_t buffer_size;
};

void kos_sha256_initialize(struct kos_sha256_context *context);
void kos_sha256_update(struct kos_sha256_context *context, const uint8_t *data, uint64_t size);
void kos_sha256_finalize(struct kos_sha256_context *context, uint8_t digest[32]);
void kos_hmac_sha256(const uint8_t *key, uint64_t key_size, const uint8_t *data, uint64_t data_size,
    uint8_t digest[32]);
bool kos_hkdf_sha256(const uint8_t *salt, uint64_t salt_size, const uint8_t *ikm,
    uint64_t ikm_size, const uint8_t *info, uint64_t info_size, uint8_t *output, uint64_t output_size);
bool kos_aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
    const uint8_t *aad, uint64_t aad_size, const uint8_t *plaintext, uint64_t size,
    uint8_t *ciphertext, uint8_t tag[16]);
bool kos_aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
    const uint8_t *aad, uint64_t aad_size, const uint8_t *ciphertext, uint64_t size,
    const uint8_t tag[16], uint8_t *plaintext);
bool kos_crypto_self_test(void);

struct kos_entropy_status {
    bool initialized;
    bool hardware_source;
    uint64_t samples;
};

bool kos_entropy_initialize(void);
bool kos_entropy_status(struct kos_entropy_status *status);
bool kos_entropy_fill(uint8_t *output, uint64_t size);

#endif
