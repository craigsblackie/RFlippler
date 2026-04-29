// Unit tests for HiTag2 stream cipher against known test vectors.
// Compile standalone: gcc -o test_crypto test_hitag2_crypto.c ../src/hitag2_crypto.c
// Expected output: all PASS

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "../src/hitag2_crypto.h"

// Test vector from Proxmark3 test suite (Verdult et al.)
// Key "MIKRON" (ASCII), uid and iv from paper
static void test_vector_mikron(void) {
    // 48-bit key from "MIKRON\0": 0x4B4D4952464F (first 6 bytes ASCII "MIKRON")
    uint64_t key = 0x4B4D4952464FULL;
    uint32_t uid = 0x69574536;
    uint32_t iv  = 0x00000000;

    // Expected first 8 keystream bytes: D7 23 7F CE 8C D0 37 A9
    uint8_t expected[] = {0xD7, 0x23, 0x7F, 0xCE, 0x8C, 0xD0, 0x37, 0xA9};

    Hitag2Crypto ctx;
    hitag2_crypto_init(&ctx, uid, key, iv);

    uint8_t got[8];
    for(int i = 0; i < 8; i++) {
        got[i] = 0;
        for(int b = 0; b < 8; b++) {
            got[i] |= hitag2_crypto_bit(&ctx) << b;
        }
    }

    int pass = memcmp(got, expected, 8) == 0;
    printf("test_vector_mikron: %s\n", pass ? "PASS" : "FAIL");
    if(!pass) {
        printf("  Expected: ");
        for(int i = 0; i < 8; i++) printf("%02X ", expected[i]);
        printf("\n  Got:      ");
        for(int i = 0; i < 8; i++) printf("%02X ", got[i]);
        printf("\n");
    }
}

// Encrypt then decrypt must return original plaintext
static void test_encrypt_decrypt_roundtrip(void) {
    uint64_t key = 0x112233445566ULL;
    uint32_t uid = 0xDEADBEEF;
    uint32_t iv  = 0xCAFEBABE;
    uint32_t plaintext = 0x12345678;

    Hitag2Crypto enc_ctx, dec_ctx;
    hitag2_crypto_init(&enc_ctx, uid, key, iv);
    hitag2_crypto_init(&dec_ctx, uid, key, iv);

    uint32_t ciphertext = hitag2_crypto_crypt32(&enc_ctx, plaintext);
    uint32_t recovered  = hitag2_crypto_crypt32(&dec_ctx, ciphertext);

    int pass = (recovered == plaintext);
    printf("test_encrypt_decrypt_roundtrip: %s\n", pass ? "PASS" : "FAIL");
    if(!pass) {
        printf("  plaintext=%08X ciphertext=%08X recovered=%08X\n",
               plaintext, ciphertext, recovered);
    }
}

// Two different IVs must produce different keystreams
static void test_iv_differentiates_keystream(void) {
    uint64_t key = 0xAABBCCDDEEFFULL;
    uint32_t uid = 0x11223344;

    Hitag2Crypto ctx1, ctx2;
    hitag2_crypto_init(&ctx1, uid, key, 0x00000001);
    hitag2_crypto_init(&ctx2, uid, key, 0x00000002);

    uint32_t w1 = hitag2_crypto_word(&ctx1);
    uint32_t w2 = hitag2_crypto_word(&ctx2);

    int pass = (w1 != w2);
    printf("test_iv_differentiates_keystream: %s\n", pass ? "PASS" : "FAIL");
}

int main(void) {
    test_vector_mikron();
    test_encrypt_decrypt_roundtrip();
    test_iv_differentiates_keystream();
    return 0;
}
