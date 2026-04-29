#pragma once
#include <stdint.h>

// HiTag2 known passwords and 48-bit crypto keys.
// Sources: Kev Sheldrake (Paxton analysis), NXP factory defaults,
//          Verdult et al. "Gone in 360 Seconds" test data.

// ── 32-bit password-mode passwords ──────────────────────────────────────────

#define HITAG2_PASSWORD_COUNT 12

static const uint32_t hitag2_default_passwords[HITAG2_PASSWORD_COUNT] = {
    0xBDF5E846u, // [0] Paxton Net2 default (Kev Sheldrake analysis)
    0x4D494B52u, // [1] "MIKR" — NXP/Philips factory default
    0x00000000u, // [2] All-zero default
    0xFFFFFFFFu, // [3] All-ones default
    0xAABBCCDDu, // [4]
    0x11223344u, // [5]
    0xDEADBEEFu, // [6]
    0xCAFEBABEu, // [7]
    0x12345678u, // [8]
    0x87654321u, // [9]
    0xA5A5A5A5u, // [10]
    0x5A5A5A5Au, // [11]
};

// ── 48-bit crypto-mode keys ──────────────────────────────────────────────────
// The full 48-bit key cannot be expressed as a 32-bit value; each entry here
// is stored as a uint64 with only bits 47..0 significant.

#define HITAG2_CRYPTO_KEY_COUNT 4

static const uint64_t hitag2_crypto_keys[HITAG2_CRYPTO_KEY_COUNT] = {
    // Paxton Net2 master key (reported by Sheldrake; verify against your system)
    0x4D494B524D49ULL, // "MIKRIMI" packed
    // NXP factory default crypto key
    0x000000000000ULL,
    // All-ones
    0xFFFFFFFFFFFFULL,
    // Common vendor key
    0xAABBCCDDEEFFULL,
};

// Names for display
static const char* const hitag2_password_names[HITAG2_PASSWORD_COUNT] = {
    "Paxton default",
    "NXP MIKR",
    "All zeros",
    "All ones",
    "AABBCCDD",
    "11223344",
    "DEADBEEF",
    "CAFEBABE",
    "12345678",
    "87654321",
    "A5A5A5A5",
    "5A5A5A5A",
};
