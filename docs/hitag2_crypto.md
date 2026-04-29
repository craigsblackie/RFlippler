# HiTag2 Stream Cipher Algorithm

Source: Verdult et al. (2012), Proxmark3, RFIDler implementations.

## Overview

The HiTag2 cipher is a 48-bit LFSR with a nonlinear output filter. Given the UID,
48-bit key, and a 32-bit IV (nonce), it produces a keystream that encrypts all tag
communication in crypto mode.

The cipher is **completely broken** as of 2012. Keys are recoverable in under 1 second
on modern hardware from a single authentication trace.

## State and Parameters

```
uid  = 32-bit tag serial number (page 0)
key  = 48-bit secret (16 bits from page 2 + 32 bits internal factory)
iv   = 32-bit random nonce from reader
lfsr = 48-bit internal state
```

## Initialization

```
1. Load lfsr[47:0]:
     lfsr[47:32] = uid[31:16]
     lfsr[31:0]  = key[15:0] | uid[15:0]    // implementation detail varies

2. For i = 0..31:
     lfsr <<= 1
     lfsr[0] = iv[i] XOR key[i+16] XOR nonlinear_filter(lfsr)

3. For i = 32..47:
     lfsr <<= 1
     lfsr[0] = nonlinear_filter(lfsr)
```

## Nonlinear Filter Function

Five sub-functions, each a lookup in a small table:

```c
// Lookup tables (from Proxmark3 hitag2.c)
static const uint32_t ht2_f4a = 0x2C79;   // 4-input function a
static const uint32_t ht2_f4b = 0x6671;   // 4-input function b
static const uint32_t ht2_f5  = 0x7907287B; // 5-input function

// Tap positions in 48-bit state (bit indices)
// Input taps to f4a: {1, 2, 4, 8}
// Input taps to f4b: {10, 16, 22, 32}
// Input tap to f5:   {2, 4, 8, 16, 32} combined with outputs of f4a, f4b

uint8_t hitag2_nonlinear(uint64_t state) {
    uint8_t x0 = (state >> 1)  & 1;
    uint8_t x1 = (state >> 2)  & 1;
    uint8_t x2 = (state >> 4)  & 1;
    uint8_t x3 = (state >> 8)  & 1;
    uint8_t y0 = (ht2_f4a >> (x3<<3 | x2<<2 | x1<<1 | x0)) & 1;

    uint8_t x4 = (state >> 10) & 1;
    uint8_t x5 = (state >> 16) & 1;
    uint8_t x6 = (state >> 22) & 1;
    uint8_t x7 = (state >> 32) & 1;
    uint8_t y1 = (ht2_f4b >> (x7<<3 | x6<<2 | x5<<1 | x4)) & 1;

    uint8_t x8 = (state >> 2)  & 1;
    uint8_t x9 = (state >> 4)  & 1;
    uint8_t x10= (state >> 8)  & 1;
    uint8_t x11= (state >> 16) & 1;
    uint8_t x12= (state >> 32) & 1;
    // f5 takes y0, y1, and three more tapped bits
    return (ht2_f5 >> (y1<<4 | y0<<3 | x12<<2 | x11<<1 | x10)) & 1;
}
```

## Keystream Generation

```c
uint8_t hitag2_crypto_bit(Hitag2Crypto* ctx) {
    uint8_t bit = hitag2_nonlinear(ctx->lfsr);
    // LFSR feedback polynomial (Galois): taps at {0,2,3,6,7,8,16,22,23,26,30,41,42,43,46,47}
    ctx->lfsr = (ctx->lfsr >> 1) | ((uint64_t)feedback_bit(ctx->lfsr) << 47);
    return bit;
}
```

## Test Vector

From Proxmark3 test suite:

```
uid = 0x69574536
key = 0x4B4D4952464F4E00  ("MIKRON\0" in ASCII, only 48 bits used)
iv  = 0x00000000

First 8 keystream bytes: D7 23 7F CE 8C D0 37 A9
```

This vector must pass before any hardware testing.

## LFSR Feedback Polynomial

48-bit maximal-length LFSR with primitive polynomial. Tap positions from Proxmark3:

```c
// Feedback bit: XOR of taps
static inline uint8_t lfsr_feedback(uint64_t state) {
    return __builtin_parityll(state & 0x0002000000060000ULL)
         ^ __builtin_parityll(state & 0x0000000000000017ULL);
    // Combined tap mask derived from primitive polynomial
}
```

Exact tap mask (bits from LSB): 0, 2, 3, 6, 7, 8, 16, 22, 23, 26, 30, 41, 42, 43, 46, 47

## Performance Note

On ARM Cortex-M4 (Flipper Zero STM32WB55):
- Single bit: ~5–10 cycles
- 32-bit word: ~160–320 cycles
- Target: complete auth handshake in <10ms total
- Optimize with `__builtin_parityll` for XOR-heavy parity — GCC emits RBIT+CLZ or similar
