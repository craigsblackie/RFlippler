#pragma once
#include <stdint.h>

// HiTag2 48-bit LFSR stream cipher.
// Cipher is cryptographically broken (Verdult 2012) but implemented for
// interoperability with existing deployed tags in crypto mode.

typedef struct {
    uint64_t lfsr; // 48-bit state (upper 16 bits unused)
} Hitag2Crypto;

// Initialize cipher from tag UID, 48-bit key, and 32-bit reader nonce (IV).
void hitag2_crypto_init(Hitag2Crypto* ctx, uint32_t uid, uint64_t key, uint32_t iv);

// Return one keystream bit and advance LFSR.
uint8_t hitag2_crypto_bit(Hitag2Crypto* ctx);

// Return 32 keystream bits (LSB first).
uint32_t hitag2_crypto_word(Hitag2Crypto* ctx);

// Encrypt/decrypt 32-bit word (XOR with keystream word).
uint32_t hitag2_crypto_crypt32(Hitag2Crypto* ctx, uint32_t data);
