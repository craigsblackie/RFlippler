#include "hitag2_reader.h"
#include <furi.h>
#include <string.h>
#include <stdlib.h>
#include <furi_hal_rfid.h>
#include <furi_hal_cortex.h>
#include <lib/toolbox/manchester_decoder.h>

#define TAG "HiTag2"

// ── HiTag2 BPLM timing (Bi-Phase Level Modulation, reader→tag) ───────────────
// T0 = 8 µs (1/125 kHz carrier period)
// Bit '0': gap ≥ 16×T0 = 128 µs;  mark ≥ 7×T0 = 56 µs
// Bit '1': gap  = 3–6×T0 = 24–48 µs; mark ≥ 7×T0 = 56 µs
//
// The Flipper antenna has τ ≈ 1000 µs which is too long for 128 µs spec gaps.
// After soldering a 15–22 Ω series resistor to the LF coil, τ drops to ~200 µs
// and these timings work.  See INVESTIGATION.md.

#define HT2_GAP_0_US    160u   // '0' bit gap (spec minimum 128 µs)
#define HT2_GAP_1_US    36u    // '1' bit gap (spec 24–48 µs)
#define HT2_MARK_US     64u    // mark duration (≥ 56 µs)

// Manchester RF/64 (tag→reader): 512 µs/bit, ±110 µs tolerance
#define HT2_HALF_US     256u
#define HT2_FULL_US     512u
#define HT2_TOL_US      110u

#define HT2_AUTH_BITS    10
#define HT2_UID_BITS     32
#define HT2_PAGE_BITS    32
#define HT2_POWERUP_MS   15

// HiTag2 read command: 0b11xxxxxx — upper 2 bits = 11, lower 6 = page address
#define HT2_CMD_READ(page)  (0xC0u | ((page) & 0x07u))

// ── Hardware register aliases ─────────────────────────────────────────────────

#define TIM1_ARR_REG   (*(volatile uint32_t*)0x40012C2CU)
#define TIM1_CCR1_REG  (*(volatile uint32_t*)0x40012C34U)

#define GPIOB_MODER     (*(volatile uint32_t*)0x48000400U)
#define GPIOB_IDR       (*(volatile uint32_t*)0x48000410U)
#define GPIOB_ODR       (*(volatile uint32_t*)0x48000414U)
#define PB13_MODER_MASK (3u << 26)
#define PB13_MODER_OUT  (1u << 26)   // GPIO output
#define PB13_MODER_AF   (2u << 26)   // TIM1_CH1N
#define PB13_BIT        (1u << 13)

#define CYCCNT (*(volatile uint32_t*)0xE0001004U)

// ── Transition log ────────────────────────────────────────────────────────────

#define TRANS_LOG_MAX 40

typedef struct {
    volatile uint32_t ts[TRANS_LOG_MAX];
    volatile int      count;
} TransLog;

// ── Comparator / Manchester context ──────────────────────────────────────────

typedef struct {
    volatile bool          active;
    volatile uint32_t      data;        // accumulated decoded bits
    volatile int           bit_count;
    volatile int           comp_ticks;
    volatile bool          done;
    volatile bool          error;
    FuriSemaphore*         sem;
    volatile ManchesterState manch_state;
    volatile uint32_t      last_cyc;
    uint32_t               cyc_per_us;
    bool                   invert_level;
    int                    target_bits;  // how many bits to collect
    TransLog               tlog;
} CompCtx;

static void comp_cb(bool level, void* ctx_ptr) {
    CompCtx* ctx = ctx_ptr;
    ctx->comp_ticks++;

    if(!ctx->active || ctx->done || ctx->error) return;
    if(ctx->invert_level) level = !level;

    uint32_t now   = CYCCNT;
    uint32_t delta = now - ctx->last_cyc;
    ctx->last_cyc  = now;
    uint32_t us    = delta / ctx->cyc_per_us;

    int idx = ctx->tlog.count;
    if(idx < TRANS_LOG_MAX) {
        ctx->tlog.ts[idx] = us;
        ctx->tlog.count   = idx + 1;
    }

    bool is_short = (us >= (HT2_HALF_US - HT2_TOL_US)) && (us <= (HT2_HALF_US + HT2_TOL_US));
    bool is_long  = (us >= (HT2_FULL_US - HT2_TOL_US)) && (us <= (HT2_FULL_US + HT2_TOL_US));

    if(us > HT2_FULL_US + HT2_TOL_US) {
        if(ctx->bit_count >= ctx->target_bits) ctx->done = true;
        else if(ctx->bit_count > 0)            ctx->error = true;
        if(ctx->done || ctx->error) furi_semaphore_release(ctx->sem);
        return;
    }
    if(!is_short && !is_long) return;

    ManchesterEvent event = (ManchesterEvent)((level ? 2u : 0u) | (is_long ? 4u : 0u));
    ManchesterState next;
    bool bit_val;
    if(manchester_advance(ctx->manch_state, event, &next, &bit_val)) {
        ctx->data = (ctx->data << 1) | (bit_val ? 1u : 0u);
        ctx->bit_count++;
        if(ctx->bit_count >= ctx->target_bits) {
            ctx->done = true;
            furi_semaphore_release(ctx->sem);
        }
    }
    ctx->manch_state = next;
}

// ── Low-level BPLM helpers ────────────────────────────────────────────────────

// Drive PB13 LOW (carrier off) for gap_us, then restore TIM1_CH1N (carrier on).
static void bplm_gap_mark(uint32_t gap_us, uint32_t mark_us) {
    GPIOB_ODR   &= ~PB13_BIT;
    GPIOB_MODER  = (GPIOB_MODER & ~PB13_MODER_MASK) | PB13_MODER_OUT;
    furi_hal_cortex_delay_us(gap_us);
    GPIOB_MODER  = (GPIOB_MODER & ~PB13_MODER_MASK) | PB13_MODER_AF;
    furi_hal_cortex_delay_us(mark_us);
}

// Send `n_bits` bits of `data` MSB-first via BPLM.
static void bplm_send(uint32_t data, int n_bits) {
    for(int i = n_bits - 1; i >= 0; i--) {
        uint32_t gap = ((data >> i) & 1u) ? HT2_GAP_1_US : HT2_GAP_0_US;
        bplm_gap_mark(gap, HT2_MARK_US);
    }
}

// Send START_AUTH: 10 × '0' bits.
static void bplm_start_auth(void) {
    for(int i = 0; i < HT2_AUTH_BITS; i++)
        bplm_gap_mark(HT2_GAP_0_US, HT2_MARK_US);
}

// ── CompCtx helpers ───────────────────────────────────────────────────────────

static CompCtx* ctx_alloc(int target_bits, bool invert, ManchesterState start) {
    CompCtx* ctx = malloc(sizeof(CompCtx));
    memset(ctx, 0, sizeof(*ctx));
    ctx->sem          = furi_semaphore_alloc(1, 0);
    ctx->manch_state  = start;
    ctx->cyc_per_us   = furi_hal_cortex_instructions_per_microsecond();
    ctx->invert_level = invert;
    ctx->target_bits  = target_bits;
    return ctx;
}

static void ctx_free(CompCtx* ctx) {
    furi_semaphore_free(ctx->sem);
    free(ctx);
}

// Arm the context for a new listen phase (call immediately before going active).
static void ctx_reset_listen(CompCtx* ctx) {
    ctx->data      = 0;
    ctx->bit_count = 0;
    ctx->done      = false;
    ctx->error     = false;
    ctx->tlog.count = 0;
    ctx->last_cyc  = CYCCNT;
    ctx->active    = true;
}

// ── Attempt counter (polarity cycling) ───────────────────────────────────────

static volatile uint32_t s_attempt = 0;

// ── Gap sweep table (diagnostic) ─────────────────────────────────────────────

static const uint32_t gap_sweep_us[]  = { 160,  250,  400,  600,  800, 1200 };
static const uint32_t mark_sweep_us[] = {  56,  150,  500,  700, 1000, 1500 };

// ── Public API — diagnostic UID read ─────────────────────────────────────────

bool hitag2_hw_read_uid(
    uint32_t* uid_out,
    int*      bits_out,
    int*      comp_out,
    int*      bplm_out,
    uint32_t  timeout_ms) {

    furi_assert(uid_out);

    uint32_t cfg    = s_attempt++;
    bool     invert = (cfg & 1u) != 0;
    ManchesterState start = (cfg & 2u) ? ManchesterStateStart0 : ManchesterStateStart1;

    uint32_t sweep_idx = cfg % 6;
    uint32_t gap_us  = gap_sweep_us[sweep_idx];
    uint32_t mark_us = mark_sweep_us[sweep_idx];

    CompCtx* ctx = ctx_alloc(HT2_UID_BITS, invert, start);

    furi_hal_rfid_comp_set_callback(comp_cb, ctx);
    furi_hal_rfid_comp_start();
    furi_hal_rfid_tim_read_start(125000.0f, 0.5f);
    furi_delay_ms(HT2_POWERUP_MS);

    uint32_t arr  = TIM1_ARR_REG;
    uint32_t ccr1 = TIM1_CCR1_REG;
    uint32_t moder_before = GPIOB_MODER;
    uint32_t moder_gap0 = 0, idr_gap0 = 0;

    int ticks_before = ctx->comp_ticks;
    int prev = ticks_before;
    int bit_ticks[HT2_AUTH_BITS];

    for(int i = 0; i < HT2_AUTH_BITS; i++) {
        GPIOB_ODR   &= ~PB13_BIT;
        GPIOB_MODER  = (GPIOB_MODER & ~PB13_MODER_MASK) | PB13_MODER_OUT;
        if(i == 0) { moder_gap0 = GPIOB_MODER; idr_gap0 = GPIOB_IDR; }
        furi_hal_cortex_delay_us(gap_us);
        GPIOB_MODER  = (GPIOB_MODER & ~PB13_MODER_MASK) | PB13_MODER_AF;
        furi_hal_cortex_delay_us(mark_us);
        int t = ctx->comp_ticks;
        bit_ticks[i] = t - prev;
        prev = t;
    }
    int ticks_after_cmd = ctx->comp_ticks;

    ctx_reset_listen(ctx);
    FuriStatus st = furi_semaphore_acquire(ctx->sem, timeout_ms);
    ctx->active = false;

    furi_hal_rfid_comp_stop();
    furi_hal_rfid_tim_read_stop();

    int listen_comp = ctx->comp_ticks - ticks_after_cmd;
    int bplm_total  = ticks_after_cmd - ticks_before;

    FURI_LOG_I(TAG, "cfg=%lu gap=%luus mark=%luus ARR=%lu CCR1=%lu",
        (unsigned long)cfg, (unsigned long)gap_us, (unsigned long)mark_us,
        (unsigned long)arr, (unsigned long)ccr1);
    FURI_LOG_I(TAG, "GPIO before=0x%08lX(PB13=%lu) gap0=0x%08lX(PB13=%lu) IDR_PB13=%lu",
        (unsigned long)moder_before,   (unsigned long)((moder_before >> 26) & 3u),
        (unsigned long)moder_gap0,     (unsigned long)((moder_gap0   >> 26) & 3u),
        (unsigned long)((idr_gap0 >> 13) & 1u));
    for(int i = 0; i < HT2_AUTH_BITS; i++)
        FURI_LOG_I(TAG, "BPLM bit%d: +%d comp", i, bit_ticks[i]);
    FURI_LOG_I(TAG, "BPLM total: %d | Listen: comp=%d bits=%d uid=0x%08lX",
        bplm_total, listen_comp, ctx->bit_count, (unsigned long)ctx->data);
    for(int i = 0; i < ctx->tlog.count; i++)
        FURI_LOG_I(TAG, "  trans[%d]=%luus", i, (unsigned long)ctx->tlog.ts[i]);
    if(ctx->tlog.count == 0) FURI_LOG_I(TAG, "  no transitions");

    if(bits_out) *bits_out = ctx->bit_count;
    if(comp_out) *comp_out = listen_comp;
    if(bplm_out) *bplm_out = bplm_total;

    bool ok = (st == FuriStatusOk && !ctx->error && ctx->done);
    if(ok) *uid_out = ctx->data;

    ctx_free(ctx);
    return ok;
}

// ── Public API — collect single nonce ────────────────────────────────────────

bool hitag2_hw_collect_nonce(uint32_t* nonce_out, uint32_t timeout_ms) {
    furi_assert(nonce_out);

    CompCtx* ctx = ctx_alloc(HT2_UID_BITS, false, ManchesterStateStart1);

    furi_hal_rfid_comp_set_callback(comp_cb, ctx);
    furi_hal_rfid_comp_start();
    furi_hal_rfid_tim_read_start(125000.0f, 0.5f);
    furi_delay_ms(HT2_POWERUP_MS);

    // Send START_AUTH with spec timing
    bplm_start_auth();

    ctx_reset_listen(ctx);
    FuriStatus st = furi_semaphore_acquire(ctx->sem, timeout_ms);
    ctx->active = false;

    furi_hal_rfid_comp_stop();
    furi_hal_rfid_tim_read_stop();

    bool ok = (st == FuriStatusOk && !ctx->error && ctx->done);
    if(ok) *nonce_out = ctx->data;

    FURI_LOG_I(TAG, "collect_nonce: ok=%d nonce=0x%08lX comp=%d bits=%d",
        ok, (unsigned long)ctx->data, ctx->comp_ticks, ctx->bit_count);

    ctx_free(ctx);
    return ok;
}

// ── Public API — full memory dump ────────────────────────────────────────────

bool hitag2_hw_dump(
    uint32_t  password,
    uint32_t* pages_out,
    int       n_pages,
    uint32_t* nonce_out,
    int*      comp_out,
    uint32_t  timeout_ms) {

    furi_assert(pages_out && n_pages > 0 && n_pages <= 8);

    CompCtx* ctx = ctx_alloc(HT2_UID_BITS, false, ManchesterStateStart1);

    furi_hal_rfid_comp_set_callback(comp_cb, ctx);
    furi_hal_rfid_comp_start();
    furi_hal_rfid_tim_read_start(125000.0f, 0.5f);
    furi_delay_ms(HT2_POWERUP_MS);

    // ── Step 1: START_AUTH ────────────────────────────────────────────────────
    bplm_start_auth();

    // ── Step 2: receive nonce (32 Manchester bits) ────────────────────────────
    ctx_reset_listen(ctx);
    FuriStatus st = furi_semaphore_acquire(ctx->sem, timeout_ms);
    ctx->active = false;

    if(st != FuriStatusOk || ctx->error || !ctx->done) {
        FURI_LOG_W(TAG, "dump: no nonce received (comp=%d bits=%d)",
            ctx->comp_ticks, ctx->bit_count);
        furi_hal_rfid_comp_stop();
        furi_hal_rfid_tim_read_stop();
        ctx_free(ctx);
        return false;
    }

    uint32_t nonce = ctx->data;
    if(nonce_out) *nonce_out = nonce;
    FURI_LOG_I(TAG, "dump: nonce=0x%08lX", (unsigned long)nonce);

    // ── Step 3: send password (32 BPLM bits, MSB first) ──────────────────────
    // For password mode, just send the plain 32-bit password.
    // The tag compares it against page 1 of its memory.
    bplm_send(password, 32);

    // Small inter-frame gap before tag responds with page 0 data
    furi_hal_cortex_delay_us(300);

    int total_comp = ctx->comp_ticks;

    // ── Step 4: read each page ────────────────────────────────────────────────
    bool ok = true;
    for(int page = 0; page < n_pages && ok; page++) {
        // Send READ command (8 bits)
        bplm_send(HT2_CMD_READ(page), 8);

        // Receive 32-bit page data
        ctx->data      = 0;
        ctx->bit_count = 0;
        ctx->done      = false;
        ctx->error     = false;
        ctx->tlog.count = 0;
        ctx->last_cyc  = CYCCNT;
        ctx->target_bits = HT2_PAGE_BITS;
        ctx->manch_state = ManchesterStateStart1;
        ctx->active    = true;

        st = furi_semaphore_acquire(ctx->sem, timeout_ms);
        ctx->active = false;

        if(st != FuriStatusOk || ctx->error || !ctx->done) {
            FURI_LOG_W(TAG, "dump: page %d read failed (bits=%d comp=%d)",
                page, ctx->bit_count, ctx->comp_ticks);
            ok = false;
            break;
        }

        pages_out[page] = ctx->data;
        FURI_LOG_I(TAG, "dump: page%d=0x%08lX", page, (unsigned long)pages_out[page]);
    }

    if(comp_out) *comp_out = ctx->comp_ticks - total_comp;

    furi_hal_rfid_comp_stop();
    furi_hal_rfid_tim_read_stop();
    ctx_free(ctx);
    return ok;
}
