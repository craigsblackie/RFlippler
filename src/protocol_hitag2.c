// HiTag2 ProtocolBase implementation for Flipper Zero LFRFID.
// Conforms to lib/toolbox/protocols/protocol.h ProtocolBase API.
// Compile into lib/lfrfid/protocols/ alongside the existing protocol_*.c files.

// NOTE: This file uses Flipper SDK types. It will not compile standalone —
// it must be built as part of the Flipper Zero firmware tree.

#include "protocol_hitag2.h"
#include "hitag2_decode.h"
#include "hitag2_encode.h"

// Flipper SDK headers (available when building in-tree)
#include <toolbox/protocols/protocol.h>
#include <toolbox/level_duration.h>
#include <furi/furi.h>
#include <lib/lfrfid/tools/t5577.h>

// Stored data layout: 32 bytes (8 pages × 4 bytes), UID in bytes 0-3.
#define HITAG2_DATA_SIZE HITAG2_MEMORY_BYTES

typedef struct {
    uint8_t       data[HITAG2_DATA_SIZE];
    Hitag2Decoder decoder;
    Hitag2Encoder encoder;
} ProtocolHiTag2;

static void* protocol_hitag2_alloc(void) {
    ProtocolHiTag2* p = malloc(sizeof(ProtocolHiTag2));
    memset(p, 0, sizeof(*p));
    return p;
}

static void protocol_hitag2_free(void* ctx) {
    free(ctx);
}

static uint8_t* protocol_hitag2_get_data(void* ctx) {
    return ((ProtocolHiTag2*)ctx)->data;
}

// Decoder ---------------------------------------------------------------

static void protocol_hitag2_decoder_start(void* ctx) {
    ProtocolHiTag2* p = ctx;
    hitag2_decoder_start(&p->decoder);
}

static bool protocol_hitag2_decoder_feed(void* ctx, bool level, uint32_t duration_us) {
    ProtocolHiTag2* p = ctx;
    Hitag2DecodeResult res = hitag2_decoder_feed(&p->decoder, level, duration_us);

    if(res == Hitag2DecodeResultOk) {
        // Copy all valid decoded pages into flat data buffer
        for(int i = 0; i < HITAG2_PAGES; i++) {
            if(p->decoder.page_valid[i]) {
                memcpy(&p->data[i * 4], p->decoder.pages[i], 4);
            }
        }
        // Signal complete when UID (page 0) is decoded
        return p->decoder.page_valid[0];
    }
    return false;
}

// Encoder (emulation) ---------------------------------------------------

static bool protocol_hitag2_encoder_start(void* ctx) {
    ProtocolHiTag2* p = ctx;
    hitag2_encoder_start(&p->encoder, p->data);
    return true;
}

static LevelDuration protocol_hitag2_encoder_yield(void* ctx) {
    ProtocolHiTag2* p = ctx;
    Hitag2LevelDuration ld = hitag2_encoder_yield(&p->encoder);

    if(ld.level == HITAG2_LEVEL_DONE) {
        // Loop: restart emulation from page 0
        hitag2_encoder_start(&p->encoder, p->data);
        ld = hitag2_encoder_yield(&p->encoder);
    }

    bool lvl = (ld.level == HITAG2_LEVEL_HIGH);
    return level_duration_make(lvl, ld.duration);
}

// Rendering -------------------------------------------------------------

static void protocol_hitag2_render_data(const uint8_t* data, FuriString* output) {
    uint32_t uid = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] <<  8) |  (uint32_t)data[3];
    uint8_t mode = data[12]; // Page 3, byte 0 (config)
    const char* mode_str = "Unknown";
    if     ((mode & 0x06) == 0x06) mode_str = "Crypto";
    else if((mode & 0x04) == 0x04) mode_str = "Password";
    else                            mode_str = "Public";

    furi_string_printf(output, "UID: %08lX\nMode: %s", uid, mode_str);
}

static void protocol_hitag2_render_brief_data(const uint8_t* data, FuriString* output) {
    uint32_t uid = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] <<  8) |  (uint32_t)data[3];
    furi_string_printf(output, "UID: %08lX", uid);
}

static void protocol_hitag2_render_uid(const uint8_t* data, FuriString* output) {
    uint32_t uid = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] <<  8) |  (uint32_t)data[3];
    furi_string_printf(output, "%08lX", uid);
}

// T5577 write support ---------------------------------------------------
// Configures T5577 to emulate HiTag2 in public mode (Manchester, RF/64).

static void protocol_hitag2_write_data(const uint8_t* data, LFRFIDT5577* t5) {
    // T5577 block 0: Manchester encoding, RF/64, maxblock = 4 (pages 0-3 + config)
    t5->block[0] = T5577_MODULATION_MANCHESTER | T5577_RF_64 | (4 << 5);

    // Pages 0-3 mapped to T5577 blocks 1-4 (MSB first, big-endian)
    for(int i = 0; i < 4; i++) {
        t5->block[i + 1] = ((uint32_t)data[i * 4 + 0] << 24) |
                            ((uint32_t)data[i * 4 + 1] << 16) |
                            ((uint32_t)data[i * 4 + 2] <<  8) |
                             (uint32_t)data[i * 4 + 3];
    }

    t5->blocks_to_write = 0b00011111; // Blocks 0-4
}

// Protocol registration -------------------------------------------------

const ProtocolBase protocol_hitag2 = {
    .data_size        = HITAG2_DATA_SIZE,
    .name             = "HiTag2",
    .manufacturer     = "NXP Semiconductors",
    .features         = LFRFIDFeatureASK,
    .validate_count   = 3,

    .alloc            = protocol_hitag2_alloc,
    .free             = protocol_hitag2_free,
    .get_data         = protocol_hitag2_get_data,

    .decoder          = {protocol_hitag2_decoder_start, protocol_hitag2_decoder_feed},
    .encoder          = {protocol_hitag2_encoder_start, protocol_hitag2_encoder_yield},

    .render_data      = protocol_hitag2_render_data,
    .render_brief_data= protocol_hitag2_render_brief_data,
    .render_uid       = protocol_hitag2_render_uid,

    .write_data       = protocol_hitag2_write_data,
};
