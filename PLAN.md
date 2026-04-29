# HiTag2 Flipper Zero Implementation Plan

## Overview

Add HiTag2 LF RFID support to the Flipper Zero firmware. HiTag2 is a 125kHz passive tag
protocol by NXP (formerly Philips Semiconductors), widely used in car immobilizers and access
control. This adds read, emulate, and write (via T5577) capability.

**References:**
- Proxmark3: `armsrc/hitag2.c`, `include/hitag.h` (RfidResearchGroup/proxmark3)
- RFIDler: `src/hitag.c`, `src/hitagcrypto.c` (AdamLaurie/RFIDler)
- Paper: "Gone in 360 Seconds" — Verdult et al., USENIX Security 2012

---

## Protocol Summary

| Property | Value |
|---|---|
| Frequency | 125 kHz |
| Modulation | ASK with Manchester encoding |
| Reader → Tag | Binary Pulse Length Modulation (BPLM) |
| Tag → Reader | Manchester encoded |
| Memory | 256 bits (8 pages × 32 bits) |
| Crypto key | 48-bit LFSR stream cipher |
| Modes | Public, Password, Crypto |

### Timing (T0 = 8 µs = 1/125kHz)

| Parameter | Value | Purpose |
|---|---|---|
| T_LOW | 6 × T0 = 48 µs | Min pulse to detect |
| T_0 (zero bit) | 20 × T0 = 160 µs | Zero bit period |
| T_1 (one bit) | 30 × T0 = 240 µs | One bit period |
| T_WAIT_1_MIN | 199 × T0 | Reader response window |
| T_PROG | 614 × T0 | Write/program delay |

### Memory Map

| Page | Contents | Notes |
|---|---|---|
| 0 | UID (32-bit) | Unique serial, factory programmed |
| 1 | RWD Password (32-bit) | Reader/writer password |
| 2 | Key high (16-bit) + reserved | Upper 16 bits of 48-bit crypto key |
| 3 | Config byte + Tag password (24-bit) | Mode select, access control |
| 4–7 | User data (16 bytes) | Freely writable |

### Commands (10-bit frames)

| Command | Code | Description |
|---|---|---|
| START_AUTH | 0x00 | Start authentication sequence |
| READ_PAGE | 0xC0\|(n<<3) | Read 32-bit page n |
| WRITE_PAGE | 0x80\|(n<<3) | Write 32-bit page n |
| HALT | 0x00 | Halt tag |

Address encoding uses complement bits: `tx[0] = 0xC0 | (page << 3) | ((page^7) >> 2)`

### Modes

1. **Public mode** — UID broadcast without auth; pages 4-7 readable
2. **Password mode** — Reader sends 32-bit password; tag unlocks on match
3. **Crypto mode** — 48-bit LFSR challenge-response; all traffic encrypted

### 48-bit Stream Cipher

- **State:** 48-bit LFSR
- **Init inputs:** 32-bit UID + 48-bit key + 32-bit IV (nonce)
- **Output:** 1 keystream bit per clock via nonlinear filter (5 lookup tables)
- **Security:** Completely broken (Verdult 2012) — key recoverable in <1s on modern hardware
- This implementation will implement the cipher for interoperability only, not as a security claim

---

## Flipper Zero Architecture

The firmware uses a modular protocol plugin architecture in `lib/lfrfid/`:

```
lib/lfrfid/
  protocols/
    protocol_*.c/h        ← One file pair per protocol — add here
    lfrfid_protocols.h/c  ← Enum + registry array — register here
  tools/
    t5577.h/c             ← Writable emulator tag support
    fsk_osc/demod.h/c     ← FSK utilities (not needed for HiTag2)
  lfrfid_worker.h/c       ← Thread orchestration (no changes needed)
lib/toolbox/protocols/
  protocol.h              ← ProtocolBase struct definition
  level_duration.h        ← Signal primitive (level + duration pairs)
```

Each protocol implements `ProtocolBase`:
- `alloc / free / get_data` — memory management
- `decoder.start / decoder.feed` — demodulate raw pulses → bytes
- `encoder.start / encoder.yield` — encode bytes → LevelDuration stream
- `render_data / render_brief_data / render_uid` — display strings
- `write_data` — populate T5577 block layout for writing

---

## File Layout for This Implementation

```
hitag2_flipper/
  PLAN.md                          ← This document
  docs/
    hitag2_protocol.md             ← Protocol reference notes
    hitag2_crypto.md               ← Stream cipher algorithm details
    timing_analysis.md             ← T0 timing, tolerances, edge cases
  src/
    protocol_hitag2.h              ← Protocol header (extern ProtocolBase)
    protocol_hitag2.c              ← Main protocol implementation
    hitag2_crypto.h                ← Stream cipher interface
    hitag2_crypto.c                ← 48-bit LFSR + nonlinear filter
    hitag2_decode.h                ← Decoder state machine header
    hitag2_decode.c                ← BPLM + Manchester decoder
    hitag2_encode.h                ← Encoder header
    hitag2_encode.c                ← Manchester encoder / LevelDuration gen
  tests/
    test_hitag2_crypto.c           ← Cipher test vectors (from Proxmark3)
    test_hitag2_decode.c           ← Decoder unit tests
    test_hitag2_encode.c           ← Encoder round-trip tests
  tools/
    hitag2_analyze.py              ← Python tool to parse raw .lfrfid files
    hitag2_sim.py                  ← Software simulation of tag for testing
```

**Files to modify in Flipper firmware repo:**
- `lib/lfrfid/protocols/lfrfid_protocols.h` — add `LFRFIDProtocolHiTag2` to enum
- `lib/lfrfid/protocols/lfrfid_protocols.c` — register `&protocol_hitag2` in array

---

## Implementation Phases

### Phase 1 — Crypto Engine (`hitag2_crypto.c/h`)

Implement the 48-bit LFSR stream cipher. This is self-contained and fully testable
against known vectors before any hardware work.

**Tasks:**
- [ ] Define `Hitag2Crypto` state struct (48-bit state, key, uid)
- [ ] Implement `hitag2_crypto_init(uid, key, iv)` — LFSR initialization sequence
- [ ] Implement `hitag2_crypto_bit()` — advance LFSR, return 1 keystream bit
- [ ] Implement `hitag2_crypto_word()` — return 32-bit keystream word
- [ ] Implement `hitag2_crypto_auth(nonce)` — full auth challenge-response
- [ ] Verify against Proxmark3 test vector: key "MIKRON" → `D7 23 7F CE 8C D0 37 A9`

**Key algorithm (from Proxmark3/RFIDler):**

```c
// LFSR feedback polynomial taps (48-bit state)
// Nonlinear filter function uses 5 lookup subtables:
static const uint32_t f_lut[] = {0x2C79, 0x6671, 0x7907287B};

// Initialization: 32 cycles mixing IV, then 16 cycles to warm up
void hitag2_crypto_init(Hitag2Crypto* ctx, uint32_t uid, uint64_t key, uint32_t iv);

// Per-bit output: nonlinear combination of LFSR taps
uint8_t hitag2_crypto_bit(Hitag2Crypto* ctx);
```

### Phase 2 — Decoder (`hitag2_decode.c/h`)

Decode raw 125kHz pulse durations into HiTag2 frames. Two decoders needed:

**2a. Tag→Reader Manchester decoder:**
- Input: `(bool level, uint32_t duration_us)` pairs from GPIO interrupts
- Detect T_0 (160µs ±20%) and T_1 (240µs ±20%) via BPLM
- Accumulate bits into shift register
- Detect valid frame start (SOF pattern)
- Validate with complement bits on page addresses

**2b. Frame parser:**
- Parse UID from page 0 response
- Parse config byte from page 3
- Determine mode (public/password/crypto)
- In crypto mode: decrypt using `hitag2_crypto_bit()` per incoming bit

**Tasks:**
- [ ] `Hitag2Decoder` state struct
- [ ] `hitag2_decoder_start()` — reset state machine
- [ ] `hitag2_decoder_feed(level, duration)` — process one pulse, return true when tag data complete
- [ ] `hitag2_decoder_get_uid()`, `hitag2_decoder_get_page(n)`
- [ ] Manchester edge detection with ±25% timing tolerance
- [ ] SOF / EOF detection

### Phase 3 — Encoder (`hitag2_encode.c/h`)

Generate the reader→tag BPLM signal and emulate tag→reader Manchester output.

**Reader→Tag BPLM encoding:**
- `0` bit: T_0 = 160µs pulse
- `1` bit: T_1 = 240µs pulse
- Commands encoded MSB-first, 10-bit frames

**Tag→Reader emulation (Manchester):**
- `0` → low-high transition at mid-clock
- `1` → high-low transition at mid-clock
- Clock period = 64 × T0 = 512µs (RF/64)

**Tasks:**
- [ ] `Hitag2Encoder` state struct
- [ ] `hitag2_encoder_start(data, len)` — load data, prepare bit stream
- [ ] `hitag2_encoder_yield()` — return next `LevelDuration` for emulation loop
- [ ] `hitag2_encoder_build_cmd(cmd, page)` — generate 10-bit reader command
- [ ] `hitag2_t5577_config()` — return T5577 block 0 config word for HiTag2 mode

### Phase 4 — Protocol Integration (`protocol_hitag2.c/h`)

Wire the decoder/encoder into `ProtocolBase` for the Flipper LFRFID worker.

**Tasks:**
- [ ] Define `HITAG2_DATA_SIZE` (at minimum: UID 4 bytes; full: 32 bytes for all 8 pages)
- [ ] Implement all `ProtocolBase` function pointers (see table above)
- [ ] `render_data`: display UID, mode, user pages
- [ ] `render_brief_data`: display UID only
- [ ] `render_uid`: UID hex string
- [ ] `write_data`: populate `LFRFIDT5577` for programming T5577 in HiTag2 public mode
- [ ] Export `const ProtocolBase protocol_hitag2`

**T5577 configuration for HiTag2 emulation:**
```c
// Block 0: Manchester, RF/64, biphase off, maxblock=2
write_ctx->block[0] = T5577_MODULATION_MANCHESTER | T5577_RF_64 | (2 << 5);
// Blocks 1-2: UID + page 1 data encoded in Manchester
```

### Phase 5 — Registry Integration

Add `protocol_hitag2` to the Flipper firmware registry.

**Tasks:**
- [ ] Add `LFRFIDProtocolHiTag2` before `LFRFIDProtocolMax` in `lfrfid_protocols.h`
- [ ] Add `[LFRFIDProtocolHiTag2] = &protocol_hitag2` in `lfrfid_protocols.c`
- [ ] Include `protocol_hitag2.h` in `lfrfid_protocols.c`
- [ ] Add source file to `lib/lfrfid/protocols/SConscript` (or CMakeLists)

### Phase 6 — Testing

**Unit tests (host, no hardware):**
- [ ] Crypto: test vectors from Proxmark3 test suite
- [ ] Decoder: feed synthetic pulse sequences, verify byte output
- [ ] Encoder: encode known UID, verify waveform is valid Manchester

**Hardware tests:**
- [ ] Read a real HiTag2 tag (car key fob, parking card) in public mode
- [ ] Confirm UID matches Proxmark3 reading of same tag
- [ ] Emulate from saved data — verify another reader accepts it
- [ ] Write HiTag2 public mode to T5577 blank tag — verify readback

**Scope / limitation:**
- Phase 1-4 implement public mode read + emulate fully
- Password mode: read password-protected pages (requires known password)
- Crypto mode: read encrypted tags requires key — implement auth handshake
  but not key recovery (that is out of scope for this implementation)

---

## Key Constants

```c
#define HITAG2_T0_US          8       // Basic time unit (microseconds)
#define HITAG2_T_LOW          (6  * HITAG2_T0_US)   // 48us  — min pulse
#define HITAG2_T_0            (20 * HITAG2_T0_US)   // 160us — zero bit
#define HITAG2_T_1            (30 * HITAG2_T0_US)   // 240us — one bit
#define HITAG2_T_WAIT_MIN     (199 * HITAG2_T0_US)  // 1592us — response window
#define HITAG2_T_PROG         (614 * HITAG2_T0_US)  // 4912us — write time

#define HITAG2_PAGES          8
#define HITAG2_PAGE_SIZE      4       // bytes
#define HITAG2_MEMORY_SIZE    32      // bytes total
#define HITAG2_UID_PAGE       0
#define HITAG2_PASSWORD_PAGE  1
#define HITAG2_KEY_PAGE       2
#define HITAG2_CONFIG_PAGE    3

// Config byte modes (page 3, byte 0)
#define HITAG2_MODE_PUBLIC    0x00
#define HITAG2_MODE_PASSWORD  0x04
#define HITAG2_MODE_CRYPTO    0x06    // Default factory config
```

---

## Scope and Limitations

| Feature | In Scope | Notes |
|---|---|---|
| Public mode read | Yes | Phase 4 |
| Public mode emulate | Yes | Phase 4 |
| Public mode write (T5577) | Yes | Phase 4 |
| Password mode read | Yes | Requires known password in UI |
| Crypto mode read | Yes | Requires known 48-bit key |
| Key recovery | No | Out of scope; use Proxmark3 for that |
| HiTag S / HiTag1 | No | Different protocols, different plan |
| Brute-force password | No | Not implemented |

---

## Dependencies

All dependencies are already present in the Flipper Zero firmware:
- `lib/toolbox/manchester.h` — Manchester decode state machine
- `lib/toolbox/bit_lib.h` — Bit manipulation utilities
- `lib/toolbox/level_duration.h` — Signal primitive
- `lib/lfrfid/tools/t5577.h` — T5577 write support
- `furi/` — RTOS, memory, string utilities

No external libraries needed.

---

## Estimated Effort

| Phase | Complexity | Est. Time |
|---|---|---|
| 1. Crypto engine | Medium | 1-2 days |
| 2. Decoder | High | 2-3 days |
| 3. Encoder | Medium | 1-2 days |
| 4. Protocol integration | Low | 1 day |
| 5. Registry + build | Low | 2 hours |
| 6. Testing | Medium | 2-3 days |
| **Total** | | **7-12 days** |
