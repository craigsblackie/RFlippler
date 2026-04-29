#pragma once
#include <stdint.h>
#include <stdbool.h>

// ── Diagnostic UID read (public mode) ───────────────────────────────────────
// Sends START_AUTH, listens for 32-bit Manchester UID.
// For use in gap-sweep / diagnostic mode. Hardware series resistor required
// for spec-compliant timing; see INVESTIGATION.md.
//
// bits_out:  Manchester bits decoded (useful when false is returned)
// comp_out:  comparator transitions during listen window
// bplm_out:  comparator transitions during BPLM command phase (~20 = field OK)
bool hitag2_hw_read_uid(
    uint32_t* uid_out,
    int*      bits_out,
    int*      comp_out,
    int*      bplm_out,
    uint32_t  timeout_ms);

// ── Full memory dump ─────────────────────────────────────────────────────────
// Protocol:
//   1. START_AUTH (10 × 0 bits BPLM)
//   2. Receive 32-bit tag nonce (Manchester)
//   3. Send 32-bit password (BPLM)  [password mode]
//      OR send crypto challenge     [crypto mode, password = 0 → auto-select]
//   4. For each page 0..n_pages-1:
//        Send READ command (BPLM) + receive 32-bit page data (Manchester)
//
// pages_out: caller-supplied array of at least n_pages uint32_t values.
// nonce_out: tag nonce captured during step 2 (useful for crack workflow).
// n_pages:   how many pages to read (HiTag2 has 8 pages, 0..7).
//
// Returns: true if all pages read successfully.
// NOTE: requires hardware series-resistor fix for spec-compliant BPLM timing.
bool hitag2_hw_dump(
    uint32_t  password,   // 32-bit password (password mode) or key LSW (crypto)
    uint32_t* pages_out,  // output array, n_pages elements
    int       n_pages,
    uint32_t* nonce_out,  // tag nonce (may be NULL)
    int*      comp_out,   // comparator transitions (may be NULL)
    uint32_t  timeout_ms);

// ── Nonce collection ─────────────────────────────────────────────────────────
// Sends START_AUTH once, waits for the 32-bit tag nonce, returns it.
// Used for building (nonce, response) pairs for the crack workflow.
bool hitag2_hw_collect_nonce(uint32_t* nonce_out, uint32_t timeout_ms);
