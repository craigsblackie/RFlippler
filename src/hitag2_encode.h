#pragma once
#include <stdint.h>
#include <stdbool.h>

// HiTag2 encoder: generates LevelDuration pairs for Flipper Zero LFRFID worker.
// Produces the tag→reader Manchester signal for emulation mode.
// Reader→tag BPLM command encoding is also provided for active mode use.

// Opaque LevelDuration type (matches Flipper toolbox definition)
typedef struct {
    uint32_t duration : 30;
    uint8_t  level    : 2;
} Hitag2LevelDuration;

#define HITAG2_LEVEL_LOW  1
#define HITAG2_LEVEL_HIGH 2
#define HITAG2_LEVEL_DONE 0

typedef struct {
    uint8_t  data[32];      // Full 32-byte tag memory image
    int      page;          // Current page being emitted
    int      bit;           // Current bit within page word
    int      half;          // Manchester half-bit (0 or 1)
    bool     done;
} Hitag2Encoder;

void hitag2_encoder_start(Hitag2Encoder* enc, const uint8_t data[32]);

// Return next LevelDuration. level == HITAG2_LEVEL_DONE signals end of stream.
Hitag2LevelDuration hitag2_encoder_yield(Hitag2Encoder* enc);

// Build 10-bit BPLM reader command frame into buf[2].
// Returns number of bits (always 10).
int hitag2_build_cmd_read(uint8_t buf[2], uint8_t page);
int hitag2_build_cmd_write(uint8_t buf[2], uint8_t page);
int hitag2_build_cmd_start_auth(uint8_t buf[2]);
