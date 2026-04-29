#pragma once
#include <stdint.h>
#include <stdbool.h>

// HiTag2 decoder state machine.
// Converts (level, duration_us) pulse pairs from 125kHz LF reader hardware
// into decoded page data. Handles public mode; crypto mode requires key.

#define HITAG2_PAGES         8
#define HITAG2_PAGE_BYTES    4
#define HITAG2_MEMORY_BYTES  32

// Config byte mode bits (page 3, byte 0)
#define HITAG2_MODE_PUBLIC   0x00
#define HITAG2_MODE_PASSWORD 0x04
#define HITAG2_MODE_CRYPTO   0x06

typedef enum {
    Hitag2DecodeResultIncomplete = 0,
    Hitag2DecodeResultOk,
    Hitag2DecodeResultError,
} Hitag2DecodeResult;

typedef struct {
    // Decoded memory pages (valid after result == Ok)
    uint8_t pages[HITAG2_PAGES][HITAG2_PAGE_BYTES];
    bool    page_valid[HITAG2_PAGES];
    uint8_t mode; // detected mode byte

    // Internal state machine (treat as opaque)
    int     state;
    uint32_t accumulator;
    int      bit_count;
    uint32_t last_duration_us;
    bool     last_level;
} Hitag2Decoder;

void hitag2_decoder_start(Hitag2Decoder* dec);

// Feed one pulse. Returns Ok when a complete valid page response is accumulated.
// Call repeatedly; pages[] filled incrementally as each page is received.
Hitag2DecodeResult hitag2_decoder_feed(Hitag2Decoder* dec, bool level, uint32_t duration_us);

// Convenience: get UID (page 0, little-endian)
uint32_t hitag2_decoder_get_uid(const Hitag2Decoder* dec);
