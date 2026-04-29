#include "hitag2_crypto.h"

// Nonlinear filter lookup tables (from Proxmark3 / Verdult et al.)
static const uint32_t ht2_f4a = 0x2C79;
static const uint32_t ht2_f4b = 0x6671;
static const uint32_t ht2_f5  = 0x7907287B;

// Tap mask for 48-bit LFSR feedback polynomial.
// Bit positions: {0,2,3,6,7,8,16,22,23,26,30,41,42,43,46,47}
#define LFSR_TAP_MASK 0x0000C40000019917ULL

static inline uint8_t lfsr_feedback(uint64_t state) {
    return (uint8_t)__builtin_parityll(state & LFSR_TAP_MASK);
}

static inline uint8_t nf_bit(uint64_t s) {
    // Tap positions for f4a: {1,2,4,8}
    uint8_t i4a = (uint8_t)(((s >> 1) & 1) | (((s >> 2) & 1) << 1) |
                             (((s >> 4) & 1) << 2) | (((s >> 8) & 1) << 3));
    uint8_t y0 = (ht2_f4a >> i4a) & 1;

    // Tap positions for f4b: {10,16,22,32}
    uint8_t i4b = (uint8_t)(((s >> 10) & 1) | (((s >> 16) & 1) << 1) |
                              (((s >> 22) & 1) << 2) | (((s >> 32) & 1) << 3));
    uint8_t y1 = (ht2_f4b >> i4b) & 1;

    // f5 taps: {y0,y1} plus {8,16,32} from state
    uint8_t i5 = (uint8_t)((y0) | (y1 << 1) | (((s >> 8)  & 1) << 2) |
                             (((s >> 16) & 1) << 3) | (((s >> 32) & 1) << 4));
    return (ht2_f5 >> i5) & 1;
}

void hitag2_crypto_init(Hitag2Crypto* ctx, uint32_t uid, uint64_t key, uint32_t iv) {
    // Seed LFSR with uid and lower 16 bits of key per NXP spec
    ctx->lfsr = ((uint64_t)uid << 16) | (key & 0xFFFF);

    // 32 mixing cycles incorporating IV and key
    for(int i = 0; i < 32; i++) {
        uint8_t out = nf_bit(ctx->lfsr);
        uint8_t fb  = lfsr_feedback(ctx->lfsr);
        ctx->lfsr = ((ctx->lfsr >> 1) & 0x0000FFFFFFFFFFFFULL) | ((uint64_t)fb << 47);
        // Mix IV bit XOR key bit XOR filter output into position 0
        uint8_t inject = ((iv >> i) & 1) ^ ((key >> (i + 16)) & 1) ^ out;
        ctx->lfsr ^= (uint64_t)inject;
    }

    // 16 warm-up cycles (no injection)
    for(int i = 0; i < 16; i++) {
        uint8_t fb = lfsr_feedback(ctx->lfsr);
        ctx->lfsr = ((ctx->lfsr >> 1) & 0x0000FFFFFFFFFFFFULL) | ((uint64_t)fb << 47);
    }
}

uint8_t hitag2_crypto_bit(Hitag2Crypto* ctx) {
    uint8_t out = nf_bit(ctx->lfsr);
    uint8_t fb  = lfsr_feedback(ctx->lfsr);
    ctx->lfsr = ((ctx->lfsr >> 1) & 0x0000FFFFFFFFFFFFULL) | ((uint64_t)fb << 47);
    return out;
}

uint32_t hitag2_crypto_word(Hitag2Crypto* ctx) {
    uint32_t word = 0;
    for(int i = 0; i < 32; i++) {
        word |= ((uint32_t)hitag2_crypto_bit(ctx)) << i;
    }
    return word;
}

uint32_t hitag2_crypto_crypt32(Hitag2Crypto* ctx, uint32_t data) {
    return data ^ hitag2_crypto_word(ctx);
}
