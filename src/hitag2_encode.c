#include "hitag2_encode.h"
#include <string.h>

// Timing constants (microseconds)
#define T0_US              8
#define BPLM_T0_US         (20 * T0_US)  // 160us — zero bit
#define BPLM_T1_US         (30 * T0_US)  // 240us — one bit
#define MANCHESTER_HALF_US (32 * T0_US)  // 256us — half of RF/64 bit period

void hitag2_encoder_start(Hitag2Encoder* enc, const uint8_t data[32]) {
    memcpy(enc->data, data, 32);
    enc->page = 0;
    enc->bit  = 31; // MSB first within each 32-bit word
    enc->half = 0;
    enc->done = false;
}

Hitag2LevelDuration hitag2_encoder_yield(Hitag2Encoder* enc) {
    if(enc->done) {
        return (Hitag2LevelDuration){.duration = 0, .level = HITAG2_LEVEL_DONE};
    }

    // Determine current bit value from page data
    int byte_idx = enc->page * 4 + (31 - enc->bit) / 8;
    int bit_idx  = 7 - ((31 - enc->bit) % 8);
    uint8_t bit_val = (enc->data[byte_idx] >> bit_idx) & 1;

    // Manchester encoding:
    //   bit 0 → first half LOW,  second half HIGH
    //   bit 1 → first half HIGH, second half LOW
    Hitag2LevelDuration ld;
    ld.duration = MANCHESTER_HALF_US;

    if(enc->half == 0) {
        ld.level = bit_val ? HITAG2_LEVEL_HIGH : HITAG2_LEVEL_LOW;
        enc->half = 1;
    } else {
        ld.level = bit_val ? HITAG2_LEVEL_LOW : HITAG2_LEVEL_HIGH;
        enc->half = 0;
        enc->bit--;
        if(enc->bit < 0) {
            enc->bit = 31;
            enc->page++;
            if(enc->page >= 8) {
                enc->done = true;
            }
        }
    }

    return ld;
}

// Command frame encoding (reader → tag direction, BPLM)
// Address complement: tx[0] = 0xC0 | (page<<3) | ((page^7)>>2)
//                    tx[1] = ((page^7)<<6)

int hitag2_build_cmd_read(uint8_t buf[2], uint8_t page) {
    buf[0] = (uint8_t)(0xC0 | ((page & 3) << 3) | (((page ^ 7) & 3) >> 2));
    buf[1] = (uint8_t)(((page ^ 7) & 3) << 6);
    return 10;
}

int hitag2_build_cmd_write(uint8_t buf[2], uint8_t page) {
    buf[0] = (uint8_t)(0x80 | ((page & 3) << 3) | (((page ^ 7) & 3) >> 2));
    buf[1] = (uint8_t)(0x40 | (((page ^ 7) & 3) << 6));
    return 10;
}

int hitag2_build_cmd_start_auth(uint8_t buf[2]) {
    buf[0] = 0x00;
    buf[1] = 0x00;
    return 10;
}
