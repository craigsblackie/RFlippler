#pragma once
#include <stdint.h>
#include <stdbool.h>

// HiTag2 48-bit LFSR stream cipher
// Reference: Verdult, Garcia, Ege — "Gone in 360 Seconds" (CCS 2012)
//            Kev Sheldrake — Paxton Net2 HiTag2 analysis

// ── Cipher state ─────────────────────────────────────────────────────────────

typedef struct {
    uint64_t state; // 48-bit LFSR (bits 47..0, 0=LSB)
} Hitag2Cipher;

// Initialise with 48-bit key, 32-bit UID, 32-bit tag nonce.
// Clocks the LFSR 64 times (32 UID + 32 nonce), XOR-ing each bit into feedback.
void hitag2_cipher_init(Hitag2Cipher* c, uint64_t key48, uint32_t uid, uint32_t tag_nonce);

// Return one keystream bit and advance the state.
uint8_t hitag2_cipher_bit(Hitag2Cipher* c);

// Generate n keystream bytes (MSB-first within each byte) into buf.
void hitag2_cipher_bytes(Hitag2Cipher* c, uint8_t* buf, int n);

// ── Authentication helpers ────────────────────────────────────────────────────

// Crypto mode: verify that (key48, uid, tag_nonce) produce tag_response.
// tag_response = first 32 keystream bits XOR'd with the UID.
// Returns true on match.
bool hitag2_crypto_verify(uint64_t key48, uint32_t uid,
                          uint32_t tag_nonce, uint32_t tag_response);

// Compute the reader's 32-bit challenge for crypto mode.
// reader_nonce is random; result is the encrypted form the reader sends.
uint32_t hitag2_crypto_reader_response(uint64_t key48, uint32_t uid,
                                       uint32_t tag_nonce, uint32_t reader_nonce);

// Password mode: reader just re-sends the 32-bit password.
static inline uint32_t hitag2_pwd_response(uint32_t password) {
    return password;
}

// ── Dictionary / crack ───────────────────────────────────────────────────────

// Try every 32-bit entry in `passwords` as a 48-bit key (zero-padded upper 16 bits)
// against a captured (uid, tag_nonce, tag_response) triple.
// Returns index of first match, or -1.  Writes found key into *key48_out if non-NULL.
int hitag2_dict_crack(
    uint32_t uid, uint32_t tag_nonce, uint32_t tag_response,
    const uint32_t* passwords, int count,
    uint64_t* key48_out);

// Online correlation / fast-crack for Paxton:
// Given pairs[] = array of (tag_nonce, tag_response) pairs collected from multiple
// auth sessions against the SAME tag (same uid + same key), attempt to recover key48.
// n_pairs: number of pairs (≥8 recommended, ≥32 for high confidence).
// uid: from page 0 of the tag (or from initial pub-mode read).
// key48_out: filled with recovered key on success.
// Returns true on success.
typedef struct {
    uint32_t tag_nonce;
    uint32_t tag_response; // first 32 keystream bits XOR uid
} Hitag2Pair;

bool hitag2_fast_crack(
    uint32_t uid,
    const Hitag2Pair* pairs, int n_pairs,
    uint64_t* key48_out);

// ── Utility ──────────────────────────────────────────────────────────────────

// Convert 32-bit Paxton "card number" from page 2 into a decimal badge number.
uint32_t paxton_badge_number(uint32_t page2_raw);
