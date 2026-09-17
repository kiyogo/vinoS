#include <stdbool.h>
#include <stdint.h>

#include <kos/crypto.h>
#include <kos/cpu.h>
#include <kos/timer.h>

static const uint32_t round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotate_right(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32 - count));
}

static uint32_t read_u32_be(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
        | ((uint32_t)data[2] << 8) | data[3];
}

static void write_u32_be(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24); data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8); data[3] = (uint8_t)value;
}

static void sha256_transform(struct kos_sha256_context *context, const uint8_t block[64]) {
    uint32_t words[64];
    for (uint64_t index = 0; index < 16; ++index) words[index] = read_u32_be(&block[index * 4]);
    for (uint64_t index = 16; index < 64; ++index) {
        uint32_t s0 = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18)
            ^ (words[index - 15] >> 3);
        uint32_t s1 = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19)
            ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    uint32_t a = context->state[0], b = context->state[1], c = context->state[2], d = context->state[3];
    uint32_t e = context->state[4], f = context->state[5], g = context->state[6], h = context->state[7];
    for (uint64_t index = 0; index < 64; ++index) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + s1 + choice + round_constants[index] + words[index];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = s0 + majority;
        h = g; g = f; f = e; e = d + temporary1; d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
}

void kos_sha256_initialize(struct kos_sha256_context *context) {
    if (context == 0) return;
    *context = (struct kos_sha256_context){ .state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u } };
}

void kos_sha256_update(struct kos_sha256_context *context, const uint8_t *data, uint64_t size) {
    if (context == 0 || (data == 0 && size != 0)) return;
    context->bit_count += size * 8;
    while (size > 0) {
        uint64_t amount = 64 - context->buffer_size;
        if (amount > size) amount = size;
        for (uint64_t index = 0; index < amount; ++index) context->buffer[context->buffer_size + index] = data[index];
        context->buffer_size += amount; data += amount; size -= amount;
        if (context->buffer_size == 64) { sha256_transform(context, context->buffer); context->buffer_size = 0; }
    }
}

void kos_sha256_finalize(struct kos_sha256_context *context, uint8_t digest[32]) {
    if (context == 0 || digest == 0) return;
    uint64_t original_bits = context->bit_count;
    context->buffer[context->buffer_size++] = 0x80;
    while (context->buffer_size != 56) {
        if (context->buffer_size == 64) { sha256_transform(context, context->buffer); context->buffer_size = 0; }
        context->buffer[context->buffer_size++] = 0;
    }
    for (uint64_t index = 0; index < 8; ++index) context->buffer[56 + index] = (uint8_t)(original_bits >> (56 - index * 8));
    sha256_transform(context, context->buffer);
    for (uint64_t index = 0; index < 8; ++index) write_u32_be(&digest[index * 4], context->state[index]);
}

void kos_hmac_sha256(const uint8_t *key, uint64_t key_size, const uint8_t *data, uint64_t data_size,
        uint8_t digest[32]) {
    uint8_t key_block[64] = {0};
    if (key_size > 64) { struct kos_sha256_context key_hash; kos_sha256_initialize(&key_hash); kos_sha256_update(&key_hash, key, key_size); kos_sha256_finalize(&key_hash, key_block); key_size = 32; }
    else for (uint64_t index = 0; index < key_size; ++index) key_block[index] = key[index];
    uint8_t inner[64], outer[64];
    for (uint64_t index = 0; index < 64; ++index) { inner[index] = key_block[index] ^ 0x36; outer[index] = key_block[index] ^ 0x5c; }
    struct kos_sha256_context context; uint8_t inner_digest[32];
    kos_sha256_initialize(&context); kos_sha256_update(&context, inner, sizeof(inner)); kos_sha256_update(&context, data, data_size); kos_sha256_finalize(&context, inner_digest);
    kos_sha256_initialize(&context); kos_sha256_update(&context, outer, sizeof(outer)); kos_sha256_update(&context, inner_digest, sizeof(inner_digest)); kos_sha256_finalize(&context, digest);
}

bool kos_hkdf_sha256(const uint8_t *salt, uint64_t salt_size, const uint8_t *ikm, uint64_t ikm_size,
        const uint8_t *info, uint64_t info_size, uint8_t *output, uint64_t output_size) {
    if (output == 0 || output_size > 255 * 32 || (ikm == 0 && ikm_size != 0) || (info == 0 && info_size != 0)) return false;
    uint8_t zero_salt[32] = {0}; uint8_t prk[32];
    kos_hmac_sha256(salt != 0 ? salt : zero_salt, salt != 0 ? salt_size : sizeof(zero_salt), ikm, ikm_size, prk);
    uint8_t previous[32] = {0}; uint8_t block[32 + 255]; uint64_t produced = 0; uint8_t counter = 1;
    while (produced < output_size) {
        uint64_t position = 0; if (counter != 1) { for (uint64_t i = 0; i < 32; ++i) block[position++] = previous[i]; }
        for (uint64_t i = 0; i < info_size; ++i) block[position++] = info[i]; block[position++] = counter;
        kos_hmac_sha256(prk, sizeof(prk), block, position, previous);
        uint64_t amount = output_size - produced < 32 ? output_size - produced : 32;
        for (uint64_t i = 0; i < amount; ++i) output[produced + i] = previous[i]; produced += amount; ++counter;
    }
    return true;
}

/* AES-128 and GCM are kept self-contained so the TLS layer does not depend on
 * a hosted crypto library.  GCM uses the byte ordering from SP 800-38D. */
static uint8_t aes_gf_mul(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if ((b & 1u) != 0) result ^= a;
        bool high = (a & 0x80u) != 0;
        a = (uint8_t)(a << 1);
        if (high) a ^= 0x1bu;
        b = (uint8_t)(b >> 1);
    }
    return result;
}

static uint8_t aes_sbox(uint8_t value) {
    if (value == 0) return 0x63;
    uint8_t inverse = 1;
    /* a^254 is the multiplicative inverse in GF(2^8). */
    for (uint16_t power = 0; power < 254; ++power) inverse = aes_gf_mul(inverse, value);
    uint8_t rotated = inverse;
    rotated ^= (uint8_t)((inverse << 1) | (inverse >> 7));
    rotated ^= (uint8_t)((inverse << 2) | (inverse >> 6));
    rotated ^= (uint8_t)((inverse << 3) | (inverse >> 5));
    rotated ^= (uint8_t)((inverse << 4) | (inverse >> 4));
    return (uint8_t)(rotated ^ 0x63u);
}

static void aes_expand_key(const uint8_t key[16], uint8_t round_keys[176]) {
    for (uint64_t i = 0; i < 16; ++i) round_keys[i] = key[i];
    uint8_t rcon = 1;
    uint64_t generated = 16;
    while (generated < 176) {
        uint8_t word[4];
        for (uint64_t i = 0; i < 4; ++i) word[i] = round_keys[generated - 4 + i];
        if ((generated & 15u) == 0) {
            uint8_t first = word[0];
            word[0] = aes_sbox(word[1]) ^ rcon;
            word[1] = aes_sbox(word[2]); word[2] = aes_sbox(word[3]); word[3] = aes_sbox(first);
            rcon = aes_gf_mul(rcon, 2);
        }
        for (uint64_t i = 0; i < 4; ++i) {
            round_keys[generated] = round_keys[generated - 16] ^ word[i];
            ++generated;
            word[i] = round_keys[generated - 4];
        }
    }
}

static void aes_encrypt_block(const uint8_t round_keys[176], const uint8_t input[16], uint8_t output[16]) {
    uint8_t state[16];
    for (uint64_t i = 0; i < 16; ++i) state[i] = input[i] ^ round_keys[i];
    for (uint8_t round = 1; round <= 10; ++round) {
        uint8_t substituted[16];
        for (uint64_t i = 0; i < 16; ++i) substituted[i] = aes_sbox(state[i]);
        for (uint8_t row = 0; row < 4; ++row) {
            for (uint8_t column = 0; column < 4; ++column) {
                state[row + 4 * column] = substituted[row + 4 * ((column + row) & 3u)];
            }
        }
        if (round != 10) {
            for (uint8_t column = 0; column < 4; ++column) {
                uint8_t *v = &state[4 * column];
                uint8_t a = v[0], b = v[1], c = v[2], d = v[3];
                v[0] = aes_gf_mul(a, 2) ^ aes_gf_mul(b, 3) ^ c ^ d;
                v[1] = a ^ aes_gf_mul(b, 2) ^ aes_gf_mul(c, 3) ^ d;
                v[2] = a ^ b ^ aes_gf_mul(c, 2) ^ aes_gf_mul(d, 3);
                v[3] = aes_gf_mul(a, 3) ^ b ^ c ^ aes_gf_mul(d, 2);
            }
        }
        for (uint64_t i = 0; i < 16; ++i) state[i] ^= round_keys[round * 16 + i];
    }
    for (uint64_t i = 0; i < 16; ++i) output[i] = state[i];
}

static void gcm_xor(uint8_t target[16], const uint8_t source[16]) {
    for (uint64_t i = 0; i < 16; ++i) target[i] ^= source[i];
}

static void gcm_multiply(uint8_t x[16], const uint8_t y[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    for (uint64_t i = 0; i < 16; ++i) v[i] = y[i];
    for (uint8_t bit = 0; bit < 128; ++bit) {
        if ((x[bit / 8] & (uint8_t)(0x80u >> (bit & 7u))) != 0) gcm_xor(z, v);
        bool lsb = (v[15] & 1u) != 0;
        for (int i = 15; i > 0; --i) v[i] = (uint8_t)((v[i] >> 1) | (v[i - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1u;
    }
    for (uint64_t i = 0; i < 16; ++i) x[i] = z[i];
}

static void gcm_ghash_block(uint8_t accumulator[16], const uint8_t hash_key[16], const uint8_t block[16]) {
    gcm_xor(accumulator, block);
    gcm_multiply(accumulator, hash_key);
}

static void gcm_ghash(const uint8_t hash_key[16], const uint8_t *aad, uint64_t aad_size,
        const uint8_t *ciphertext, uint64_t size, uint8_t result[16]) {
    for (uint64_t i = 0; i < 16; ++i) result[i] = 0;
    for (uint64_t offset = 0; offset < aad_size; offset += 16) {
        uint8_t block[16] = {0}; uint64_t amount = aad_size - offset < 16 ? aad_size - offset : 16;
        for (uint64_t i = 0; i < amount; ++i) block[i] = aad[offset + i];
        gcm_ghash_block(result, hash_key, block);
    }
    for (uint64_t offset = 0; offset < size; offset += 16) {
        uint8_t block[16] = {0}; uint64_t amount = size - offset < 16 ? size - offset : 16;
        for (uint64_t i = 0; i < amount; ++i) block[i] = ciphertext[offset + i];
        gcm_ghash_block(result, hash_key, block);
    }
    uint8_t lengths[16] = {0};
    uint64_t aad_bits = aad_size * 8, ciphertext_bits = size * 8;
    for (uint64_t i = 0; i < 8; ++i) {
        lengths[7 - i] = (uint8_t)(aad_bits >> (i * 8));
        lengths[15 - i] = (uint8_t)(ciphertext_bits >> (i * 8));
    }
    gcm_ghash_block(result, hash_key, lengths);
}

static void gcm_increment_counter(uint8_t counter[16]) {
    for (int i = 15; i >= 12; --i) { if (++counter[i] != 0) break; }
}

bool kos_aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *aad,
        uint64_t aad_size, const uint8_t *plaintext, uint64_t size, uint8_t *ciphertext, uint8_t tag[16]) {
    if (key == 0 || nonce == 0 || (aad == 0 && aad_size != 0) || (plaintext == 0 && size != 0)
        || (ciphertext == 0 && size != 0) || tag == 0 || aad_size > UINT64_MAX / 8 || size > UINT64_MAX / 8) return false;
    uint8_t round_keys[176], hash_key[16] = {0}, counter[16] = {0}, stream[16];
    aes_expand_key(key, round_keys); aes_encrypt_block(round_keys, hash_key, hash_key);
    for (uint64_t i = 0; i < 12; ++i) counter[i] = nonce[i]; counter[15] = 1;
    for (uint64_t offset = 0; offset < size; offset += 16) {
        gcm_increment_counter(counter); aes_encrypt_block(round_keys, counter, stream);
        uint64_t amount = size - offset < 16 ? size - offset : 16;
        for (uint64_t i = 0; i < amount; ++i) ciphertext[offset + i] = plaintext[offset + i] ^ stream[i];
    }
    uint8_t accumulator[16]; gcm_ghash(hash_key, aad, aad_size, ciphertext, size, accumulator);
    uint8_t j0[16] = {0}; for (uint64_t i = 0; i < 12; ++i) j0[i] = nonce[i]; j0[15] = 1;
    aes_encrypt_block(round_keys, j0, tag); gcm_xor(tag, accumulator);
    return true;
}

bool kos_aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *aad,
        uint64_t aad_size, const uint8_t *ciphertext, uint64_t size, const uint8_t tag[16], uint8_t *plaintext) {
    if (key == 0 || nonce == 0 || (aad == 0 && aad_size != 0) || (ciphertext == 0 && size != 0)
        || tag == 0 || (plaintext == 0 && size != 0) || aad_size > UINT64_MAX / 8 || size > UINT64_MAX / 8) return false;
    uint8_t round_keys[176], hash_key[16] = {0}, accumulator[16], expected[16];
    aes_expand_key(key, round_keys); aes_encrypt_block(round_keys, hash_key, hash_key);
    gcm_ghash(hash_key, aad, aad_size, ciphertext, size, accumulator);
    uint8_t j0[16] = {0}; for (uint64_t i = 0; i < 12; ++i) j0[i] = nonce[i]; j0[15] = 1;
    aes_encrypt_block(round_keys, j0, expected); gcm_xor(expected, accumulator);
    uint8_t mismatch = 0; for (uint64_t i = 0; i < 16; ++i) mismatch |= expected[i] ^ tag[i];
    if (mismatch != 0) return false;
    uint8_t counter[16] = {0}, stream[16]; for (uint64_t i = 0; i < 12; ++i) counter[i] = nonce[i]; counter[15] = 1;
    for (uint64_t offset = 0; offset < size; offset += 16) {
        gcm_increment_counter(counter); aes_encrypt_block(round_keys, counter, stream);
        uint64_t amount = size - offset < 16 ? size - offset : 16;
        for (uint64_t i = 0; i < amount; ++i) plaintext[offset + i] = ciphertext[offset + i] ^ stream[i];
    }
    return true;
}

static bool cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    return (ecx & (1u << 30)) != 0;
}

static bool rdrand64(uint64_t *value) {
    unsigned char success;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*value), "=qm"(success) : : "cc");
    return success != 0;
}

static struct kos_entropy_status entropy;

bool kos_entropy_initialize(void) {
    entropy = (struct kos_entropy_status){ .initialized = true, .hardware_source = cpu_has_rdrand() };
    return true;
}

bool kos_entropy_status(struct kos_entropy_status *status) { if (status == 0) return false; *status = entropy; return true; }

bool kos_entropy_fill(uint8_t *output, uint64_t size) {
    if (!entropy.initialized || output == 0) return false;
    for (uint64_t index = 0; index < size; ) {
        uint64_t value = 0;
        if (!entropy.hardware_source || !rdrand64(&value)) return false;
        for (uint64_t byte = 0; byte < 8 && index < size; ++byte, ++index) output[index] = (uint8_t)(value >> (byte * 8));
        ++entropy.samples;
    }
    return true;
}

bool kos_crypto_self_test(void) {
    static const uint8_t expected[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    struct kos_sha256_context context; uint8_t digest[32]; const uint8_t message[] = "abc";
    kos_sha256_initialize(&context); kos_sha256_update(&context, message, 3); kos_sha256_finalize(&context, digest);
    for (uint64_t i = 0; i < sizeof(digest); ++i) if (digest[i] != expected[i]) return false;
    uint8_t expanded[42]; static const uint8_t info[] = "info";
    if (!kos_hkdf_sha256(0, 0, message, 3, info, sizeof(info) - 1, expanded, sizeof(expanded))) return false;
    static const uint8_t zero_key[16] = {0};
    static const uint8_t zero_nonce[12] = {0};
    static const uint8_t zero_plaintext[16] = {0};
    static const uint8_t expected_ciphertext[16] = {
        0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
        0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78 };
    static const uint8_t expected_tag[16] = {
        0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
        0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf };
    uint8_t ciphertext[16], tag[16], plaintext[16];
    if (!kos_aes128_gcm_encrypt(zero_key, zero_nonce, 0, 0, zero_plaintext, sizeof(zero_plaintext), ciphertext, tag)) return false;
    for (uint64_t i = 0; i < 16; ++i) if (ciphertext[i] != expected_ciphertext[i] || tag[i] != expected_tag[i]) return false;
    return kos_aes128_gcm_decrypt(zero_key, zero_nonce, 0, 0, ciphertext, sizeof(ciphertext), tag, plaintext);
}
