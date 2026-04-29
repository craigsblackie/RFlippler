#include "hitag2_decode.h"
#include <string.h>

// Timing constants (microseconds)
#define T0_US       8
#define T_0_US      (20 * T0_US)   // 160us — zero bit period
#define T_1_US      (30 * T0_US)   // 240us — one bit period
#define T_TOLERANCE_PCT 20          // ±20% tolerance on bit periods

#define IN_RANGE(v, center, pct) \
    ((v) >= ((center) - (center) * (pct) / 100) && \
     (v) <= ((center) + (center) * (pct) / 100))

// Manchester clock period for tag→reader direction (RF/64)
#define MANCHESTER_CLK_US (64 * T0_US) // 512us per bit

typedef enum {
    STATE_IDLE = 0,
    STATE_SOF,          // Waiting for start-of-frame sync
    STATE_COLLECTING,   // Accumulating Manchester bits for a page response
    STATE_DONE,
} DecoderState;

void hitag2_decoder_start(Hitag2Decoder* dec) {
    memset(dec, 0, sizeof(*dec));
    dec->state = STATE_IDLE;
}

// Decode one Manchester half-period into a bit value.
// Returns -1 if duration does not match expected clocks.
static int decode_manchester_bit(bool level, uint32_t duration_us) {
    // Manchester: one bit = two half-periods of MANCHESTER_CLK_US/2 each
    uint32_t half = MANCHESTER_CLK_US / 2;
    if(!IN_RANGE(duration_us, half, T_TOLERANCE_PCT)) return -1;
    // Convention: high-then-low half = 1, low-then-high half = 0
    // (level here is the level being presented for this duration)
    return (int)level;
}

Hitag2DecodeResult hitag2_decoder_feed(Hitag2Decoder* dec, bool level, uint32_t duration_us) {
    switch((DecoderState)dec->state) {
    case STATE_IDLE:
        // Detect SOF: tag responds after START_AUTH command with its UID.
        // The first transition into the response window triggers collection.
        // A rising edge after the reader command quiesces starts the tag response.
        if(level && duration_us > 100) {
            dec->state = STATE_SOF;
            dec->bit_count = 0;
            dec->accumulator = 0;
        }
        break;

    case STATE_SOF:
        // Collect 32 Manchester bits for page 0 (UID)
        // Falls through to COLLECTING logic
        dec->state = STATE_COLLECTING;
        dec->bit_count = 0;
        dec->accumulator = 0;
        // fall through

    case STATE_COLLECTING: {
        int bit = decode_manchester_bit(level, duration_us);
        if(bit < 0) {
            // Timing error — restart
            dec->state = STATE_IDLE;
            return Hitag2DecodeResultError;
        }
        dec->accumulator = (dec->accumulator >> 1) | ((uint32_t)bit << 31);
        dec->bit_count++;

        if(dec->bit_count == 32) {
            // Determine which page we just received based on how many pages done
            int page = 0;
            for(int i = 0; i < HITAG2_PAGES; i++) {
                if(!dec->page_valid[i]) { page = i; break; }
            }
            uint32_t word = dec->accumulator;
            dec->pages[page][0] = (word >> 24) & 0xFF;
            dec->pages[page][1] = (word >> 16) & 0xFF;
            dec->pages[page][2] = (word >>  8) & 0xFF;
            dec->pages[page][3] = (word >>  0) & 0xFF;
            dec->page_valid[page] = true;

            if(page == 3) {
                dec->mode = dec->pages[3][0];
            }

            dec->bit_count = 0;
            dec->accumulator = 0;
            dec->state = STATE_IDLE; // Ready for next command/response cycle
            return Hitag2DecodeResultOk;
        }
        break;
    }

    case STATE_DONE:
        break;
    }

    return Hitag2DecodeResultIncomplete;
}

uint32_t hitag2_decoder_get_uid(const Hitag2Decoder* dec) {
    const uint8_t* p = dec->pages[0];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
