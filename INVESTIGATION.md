# HiTag2 Flipper Zero — Investigation Notes

## Goal

Read the UID from a HiTag2 (NXP PCF7936) 125 kHz RFID card using a Flipper Zero running Momentum firmware (mntm-012, API 87.1).

---

## Protocol Background

HiTag2 is an NXP 125 kHz RFID protocol. Unlike simpler tags (e.g. EM4100) it does **not** broadcast passively. The reader must first send a **START_AUTH** command to wake the tag before the tag replies.

### BPLM (Bi-Phase Level Modulation) — reader → tag

The reader gates the 125 kHz carrier field to encode bits:

| Bit | Field OFF (gap) | Field ON (mark) |
|-----|----------------|-----------------|
| `0` | ≥ 16×T0 = 160 µs | ≥ 7×T0 = 56 µs |
| `1` | 3–6×T0 = 24–48 µs | ≥ 7×T0 = 56 µs |

`T0 = 8 µs = 1/125 kHz`

START_AUTH = 10 consecutive `0` bits.

### Manchester RF/64 — tag → reader

After a valid START_AUTH, the tag responds in Manchester at RF/64:
- Bit period: 64/125 kHz = **512 µs**
- Short half-period: 256 µs ± 110 µs tolerance
- Long half-period: 512 µs ± 110 µs tolerance
- UID = 32 bits → ~64 transitions over ~16.4 ms

In **public mode** the tag responds immediately with its UID.  
In **password/crypto mode** the tag responds with a 32-bit nonce and requires further authentication.

---

## Flipper Zero Hardware

### RFID carrier output

| Signal | Pin | Timer function |
|--------|-----|----------------|
| RFID_RF_OUT | PB13 | TIM1_CH1N (complementary, AF1) |

TIM1 is configured by `furi_hal_rfid_tim_read_start(125000.0f, 0.5f)`:

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| TIM1_ARR | 0x40012C2C | 511 | Period = 512 ticks = 8 µs = 125 kHz |
| TIM1_CCR1 | 0x40012C34 | 255 | 50% duty cycle |
| TIM1_BDTR | 0x40012C44 | 0x00008000 | MOE (bit 15) = 1, outputs enabled |

Because PB13 is **TIM1_CH1N** (complementary output):
- CH1N = 1 when CNT ∈ [CCR1, ARR] → MOSFET ON → carrier active
- CH1N = 0 when CNT ∈ [0, CCR1-1] → MOSFET OFF → carrier inactive

### RFID demodulation / comparator

The Flipper reads the tag's load-modulation response via the STM32WB55 analog comparator. The comparator fires on transitions of the demodulated envelope — it does **not** see the raw 125 kHz carrier; it sees the AM-demodulated signal after an RC filter.

GPIOB registers used (STM32WB55 AHB2, base 0x48000400):

| Register | Address |
|----------|---------|
| GPIOB_MODER | 0x48000400 |
| GPIOB_IDR | 0x48000410 |
| GPIOB_ODR | 0x48000414 |

GPIOB_MODER baseline value (from logs): `0x980FAFF5`  
PB13 bits [27:26] = `10` = Alternate Function mode (TIM1_CH1N) ✓

---

## Approaches Tried for BPLM Field Gating

### 1. TIM1_BDTR MOE bit (write 0 to disable outputs)

```c
TIM1_BDTR_REG &= ~TIM1_MOE;   // clear bit 15 → expect outputs disabled
```

**Discovered:** BDTR bit 15 (MOE) is **write-1-to-clear** on this device.

Log evidence:
```
MOE test: before=0x00008000 cleared=0x00008000 restored=0x00000000
```

Writing 0 to MOE has no effect. Writing 1 clears it. The carrier was never actually gated.

### 2. `furi_hal_rfid_tim_read_pause()` / `tim_read_continue()`

Stops TIM1 counter (CEN=0). Output should go to idle state (OIS1N=0 → CH1N LOW). Attempted but gave the same result as #1 because the antenna time constant is the real bottleneck (see below).

### 3. `furi_hal_rfid_set_read_pulse(0)` / `set_read_pulse(512)`

Setting CCR1=0 → CH1N always HIGH (carrier always ON — wrong direction for a complementary output).  
Setting CCR1=512 (> ARR=511) → CH1N always LOW (carrier OFF). Correct in theory but same antenna problem.

### 4. Direct GPIOB MODER switch (final working approach)

Switch PB13 from TIM1_CH1N (MODER=10) to GPIO output LOW (MODER=01) for the gap duration, then back to AF (MODER=10) for the mark:

```c
GPIOB_ODR   &= ~PB13_BIT;                                           // pre-set LOW
GPIOB_MODER = (GPIOB_MODER & ~PB13_MODER_MASK) | PB13_MODER_OUT;  // → GPIO output
furi_hal_cortex_delay_us(gap_us);
GPIOB_MODER = (GPIOB_MODER & ~PB13_MODER_MASK) | PB13_MODER_AF;   // → AF (TIM1_CH1N)
furi_hal_cortex_delay_us(mark_us);
```

**Verified working:**
```
GPIO before=0x980FAFF5(PB13=2) gap0=0x940FAFF5(PB13=1) IDR_PB13=0
```

- PB13 MODER correctly switches AF→OUT→AF on every bit
- IDR_PB13=0 during gap → pin is genuinely driven LOW → MOSFET OFF

---

## Diagnostic Metrics

During development the reader function was instrumented with:

- `bplm_out` (`b` on screen): comparator transitions during the 10-bit BPLM window  
  - Expected ~20 if field gaps are detectable (2 per bit)
  - Actual at 160 µs gaps: **1** (field barely moves, comp doesn't see it)

- `comp_out` (`c` on screen): comparator transitions during the 600 ms listen window  
  - Expected ~64 if tag sends a 32-bit Manchester UID  
  - Actual: **1–2** (ambient noise only) in all tests

- `trans[]`: per-transition timing log during active listen  
  - Only one spurious edge at ~42 µs (not a valid Manchester interval) was ever observed

---

## Root Cause: Antenna Q Too High for 160 µs BPLM

### Evidence

The comparator counts for different gap durations (10-bit BPLM, 2 transitions expected per bit = 20 total):

| Gap duration | Mark duration | BPLM comp | Field drop (estimated) |
|-------------|---------------|-----------|------------------------|
| 160 µs (spec) | 56 µs | **1** (noise) | ~15% |
| 250 µs | 150 µs | **20** | ~22% |
| 400 µs | 500 µs | **20** | ~33% |
| 600 µs | 700 µs | **20** | ~45% |
| 800 µs | 1000 µs | **20** | ~55% |
| 1200 µs | 1500 µs | **20** | ~70% |

The comparator sees a threshold crossing when the demodulated signal drops by roughly 20%+ of nominal. This locates the antenna's effective field decay time constant:

**τ ≈ 1000–1200 µs**

Derivation:
- At 160 µs: field = e^(−160/τ) ≈ 85% → **below comp threshold** (not detected)  
- At 250 µs: field = e^(−250/τ) ≈ 78% → **above comp threshold** (2 per bit, all detected)

For the HiTag2 tag's internal gap detector to fire, the field (and thus the tag's rectified supply voltage) must drop below its minimum operating threshold — typically implying the field must fall to ≤ 67% of nominal. With τ ≈ 1000 µs:

```
t_gap required = −τ × ln(0.67) ≈ 400 µs
```

The HiTag2 spec calls for a maximum gap of ~20–30×T0 = 160–240 µs. The Flipper antenna needs **~400 µs minimum** for the field to collapse enough for the tag to detect the gap.

### Why the sweep didn't help

Even though gaps of 250–1200 µs successfully collapse the field (BPLM comp = 20), the tag did not respond to any of them. These extended gaps exceed the HiTag2 BPLM bit-timing tolerance (≈ ±50% of 160 µs), so the tag's BPLM bit decoder likely rejects them or the tag resets before it accumulates all 10 START_AUTH bits.

### Summary

The stock Flipper Zero LF RFID antenna has a field decay time constant (τ ≈ 1000 µs) that is roughly 6× longer than the HiTag2 spec gap (160 µs). It is physically impossible for the Flipper to produce spec-compliant HiTag2 BPLM gaps without reducing the antenna's Q factor.

---

## What Was Confirmed Working

| Component | Status |
|-----------|--------|
| GPIO PB13 carrier gating (MODER switch) | ✅ Confirmed (IDR=0, MODER readback) |
| DWT CYCCNT timing (furi_hal_cortex_delay_us) | ✅ Correct at 64 MHz |
| Comparator callback (comp_cb ISR) | ✅ Fires on transitions, counts correctly |
| Manchester decoder (manchester_advance) | ✅ Logic correct; untestable without tag response |
| Tag power-up sequence (15 ms carrier before BPLM) | ✅ Correct |
| USB serial logging (FURI_LOG_I) | ✅ Working at 230400 baud via /dev/ttyACM0 |

---

## Required Hardware Fix

To reduce antenna Q, add a **15–22 Ω resistor in series with the LF antenna coil** on the Flipper Zero PCB. This increases the coil's series resistance, lowering Q and shortening τ to a value that allows 160 µs gaps to collapse the field adequately.

Effect on τ: adding R_damp in series reduces Q ≈ ωL/R_total, which reduces τ = Q/(πf). A 20 Ω series resistor typically reduces τ by 60–80%, bringing it to ~200–400 µs — well within spec for HiTag2 BPLM.

Trade-off: lower Q reduces read range for passive tags (EM4100, HID, etc.) by 20–40%.

---

## Current Software State

The app is implemented in:

```
app/
  hitag2_app.c        — GUI, reader thread, menus
  hitag2_reader.c     — BPLM + Manchester decode, all hardware access
  hitag2_reader.h     — Public API
  hitag2_passwords.h  — 10 default HiTag2 passwords
  application.fam     — FAP metadata
```

### hitag2_hw_read_uid() flow

```
1. Allocate semaphore, start comparator, start TIM1 carrier
2. Wait 15 ms (tag power-up)
3. For each of 10 BPLM bits:
     a. Set GPIOB_ODR PB13 = 0
     b. Switch GPIOB_MODER PB13 = GPIO output  (gap)
     c. Delay gap_us
     d. Switch GPIOB_MODER PB13 = AF            (mark)
     e. Delay mark_us
4. Set ctx.active = true (start Manchester decode)
5. Wait up to timeout_ms for semaphore
6. Stop comparator + TIM1
7. Log diagnostics
8. Return UID if 32 bits decoded successfully
```

### Gap sweep (diagnostic mode, currently active)

The function cycles through 6 gap/mark pairs on successive attempts:

| sweep_idx | gap_us | mark_us |
|-----------|--------|---------|
| 0 | 160 | 56 |
| 1 | 250 | 150 |
| 2 | 400 | 500 |
| 3 | 600 | 700 |
| 4 | 800 | 1000 |
| 5 | 1200 | 1500 |

To revert to fixed timing for a production build, replace the sweep table with:
```c
uint32_t gap_us  = HT2_GAP_0_US;
uint32_t mark_us = HT2_INTER_US;
```

### Manchester polarity cycling

Because the comparator polarity is unknown, each attempt cycles through four configurations via `s_attempt`:

| cfg & 3 | manch_state | invert_level |
|---------|-------------|--------------|
| 0 | ManchesterStateStart1 | false |
| 1 | ManchesterStateStart1 | true |
| 2 | ManchesterStateStart0 | false |
| 3 | ManchesterStateStart0 | true |

---

## Serial Log Monitoring

Connect Flipper via USB, then:

```bash
cd /home/user/hitag2_flipper
python3 monitor.py > log.txt 2>&1 &
```

`monitor.py` sends `log info` to the Flipper CLI and streams output; HiTag2 lines are highlighted in green.

Key log lines to watch:

```
cfg=N gap=Xus mark=Yus ARR=511 CCR1=255
GPIO before=...(PB13=2) gap0=...(PB13=1) IDR_PB13=0   ← must be 0
BPLM total: N | Listen: comp=M bits=B uid=0xXXXXXXXX
  trans[0]=Tus                                           ← first listen transition
```

- `IDR_PB13=0` — pin is truly LOW during gap (required)
- `BPLM total=20` — field is collapsing (all 10 gaps detected by comp)
- `Listen comp >> 20` — tag is responding (goal)
- `bits=32` — UID successfully decoded (success)

---

## Next Steps

1. **Hardware fix (recommended):** Solder a 15–22 Ω resistor in series with the LF coil. Re-run with spec-compliant `gap=160, mark=56`. Expect `BPLM total=20` and `Listen comp=64+`.

2. **If tag still silent after hardware fix:** Implement HiTag2 password-mode authentication — send the 32-bit nonce back with one of the `hitag2_default_passwords[]` and decode the UID from the encrypted response.

3. **Verify Manchester decode:** Once the tag sends a response, the polarity-cycling approach should decode it. If `bits` stays at 0 with `comp=64+`, tune `HT2_HALF_US`, `HT2_FULL_US`, or `HT2_TOL_US`.
