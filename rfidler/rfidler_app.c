#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG_LOG      "RFIDler"
#define UART_BAUD    115200
#define RX_BUF_SZ    512
#define RESP_SZ      512
#define TIMEOUT_MS   5000

#define MENU_VISIBLE  4
#define ITEM_H        9
#define LIST_TOP      14
#define DUMP_VISIBLE  4
#define DUMP_MAX      12
#define SAVED_MAX     24
#define SAVED_VISIBLE 4
#define SAVE_DIR      "/ext/apps/rfidler"

// Known Paxton / HiTag2 passwords for the sweep mode
// Source: Kev Sheldrake Paxton analysis + NXP factory defaults
#define PAXTON_N_PASS 6
static const char* const PAXTON_PASSWORDS[PAXTON_N_PASS] = {
    "BDF5E846",  // Paxton Net2 default (Sheldrake)
    "4D494B52",  // "MIKR" NXP factory default
    "00000000",
    "FFFFFFFF",
    "DEADBEEF",
    "CAFEBABE",
};
static const char* const PAXTON_PASS_NAMES[PAXTON_N_PASS] = {
    "Paxton default",
    "NXP MIKR",
    "All zeros",
    "All ones",
    "DEADBEEF",
    "CAFEBABE",
};

// ── Tag table ─────────────────────────────────────────────────────────────────

typedef struct {
    const char* name;
    const char* cmd_select;
    const char* cmd_auth;
    const char* cmd_read;
    bool        is_dump;
    bool        sweep_pass;
    bool        is_file_browser; // opens saved-dump list, no serial needed
} TagCfg;

static const TagCfg TAGS[] = {
    { "HiTag2 UID",      "SET TAG HITAG2\r\n", NULL,                 "UID\r\n",      false, false, false },
    { "Paxton Auto",     "SET TAG HITAG2\r\n", NULL,                 "DUMP 0 7\r\n", true,  true,  false },
    { "Paxton (default)","SET TAG HITAG2\r\n", "LOGIN BDF5E846\r\n", "DUMP 0 7\r\n", true,  false, false },
    { "EM4100 / EM4102", "SET TAG EM4100\r\n", NULL,                 "UID\r\n",      false, false, false },
    { "HID 26-bit",      "SET TAG HID26\r\n",  NULL,                 "UID\r\n",      false, false, false },
    { "Indala",          "SET TAG INDALA\r\n", NULL,                 "UID\r\n",      false, false, false },
    { "Test connection", NULL,                 NULL,                 NULL,           false, false, false },
    { "View Saved",      NULL,                 NULL,                 NULL,           false, false, true  },
};
#define TAG_COUNT ((int)(sizeof(TAGS) / sizeof(TAGS[0])))

// ── App ───────────────────────────────────────────────────────────────────────

typedef enum { StateIdle, StateBusy, StateDone, StateError, StateDump, StateSavedList } AppState;

typedef struct {
    Gui*                  gui;
    ViewPort*             vp;
    FuriMessageQueue*     eq;
    FuriStreamBuffer*     rxs;
    FuriHalSerialHandle*  serial;
    FuriThread*           worker;
    volatile AppState     state;
    int                   sel;
    int                   menu_scroll;
    const TagCfg*         cur;
    char                  result[48];
    char                  result2[48];
    char                  dump_lines[DUMP_MAX][20];
    int                   dump_count;
    int                   dump_scroll;
    int                   sweep_pass_idx;
    char                  sweep_pass_name[24];
    uint32_t              badge_number;
    // Saved-dump browser
    char                  saved_names[SAVED_MAX][20]; // UID stem (no .txt)
    int                   saved_count;
    int                   saved_sel;
    int                   saved_scroll;
} App;

// ── Serial ────────────────────────────────────────────────────────────────────

static void serial_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent ev, void* ctx) {
    App* app = ctx;
    if(ev & FuriHalSerialRxEventData)
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t b = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(app->rxs, &b, 1, 0);
        }
}

static void flush_rx(App* app) {
    uint8_t b;
    while(furi_stream_buffer_receive(app->rxs, &b, 1, 0)) {}
}


static void serial_tx(App* app, const char* cmd) {
    furi_hal_serial_tx(app->serial, (const uint8_t*)cmd, strlen(cmd));
}

static bool ihas(const char* hay, const char* needle) {
    size_t nl = strlen(needle), hl = strlen(hay);
    if(nl > hl) return false;
    for(size_t i = 0; i <= hl - nl; i++) {
        bool ok = true;
        for(size_t j = 0; j < nl; j++)
            if(tolower((unsigned char)hay[i+j]) != tolower((unsigned char)needle[j])) { ok=false; break; }
        if(ok) return true;
    }
    return false;
}

// Extract meaningful content from a RFIDler response — skips command echo
// (first line) and returns the first non-empty printable line.
static void extract_msg(const char* resp, char* out, size_t out_sz) {
    // Skip past the first newline (command echo)
    const char* p = strchr(resp, '\n');
    p = p ? p + 1 : resp;
    // Copy first printable line
    size_t i = 0;
    while(*p && i < out_sz - 1) {
        if(*p == '\r' || *p == '\n') { if(i > 0) break; p++; continue; }
        if(*p >= 0x20 && *p < 0x7F) out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    if(i == 0) strncpy(out, "no response", out_sz - 1);
}

static bool parse_uid(const char* buf, char* out9) {
    const char* p = buf;
    while(*p) {
        while(*p && !isxdigit((unsigned char)*p)) p++;
        const char* s = p;
        while(isxdigit((unsigned char)*p)) p++;
        if((size_t)(p - s) == 8) { memcpy(out9, s, 8); out9[8]='\0'; return true; }
    }
    return false;
}

// Parse DUMP response lines of the form "N: AABBCCDD".
// Collects runs of exactly 8 hex chars (page data), ignoring shorter runs
// (page number digits) and any other non-hex chars.
static int parse_dump(const char* buf, char lines[][20], int max) {
    int count = 0;
    const char* p = buf;
    while(*p && count < max) {
        while(*p && !isxdigit((unsigned char)*p)) p++;
        const char* s = p;
        while(isxdigit((unsigned char)*p)) p++;
        if((size_t)(p - s) == 8) {
            snprintf(lines[count], 20, "B%d: %.*s", count, 8, s);
            count++;
        }
    }
    return count;
}

// ── Worker ────────────────────────────────────────────────────────────────────

// Decode Paxton badge number from page 2 (lower 24 bits, big-endian).
static uint32_t decode_badge(const char lines[][20], int count) {
    // Page 2 is dump line index 2
    if(count < 3) return 0;
    // Each dump line is "B%d: %08X"
    unsigned long val = 0;
    const char* p = lines[2];
    while(*p && *p != ':') p++;
    if(*p == ':') p++;
    while(*p == ' ') p++;
    if(sscanf(p, "%lx", &val) == 1)
        return (uint32_t)(val & 0x00FFFFFFul);
    return 0;
}

static void save_dump(App* app) {
    if(app->dump_count == 0) return;
    char uid[9] = "00000000";
    const char* dp = app->dump_lines[0];
    while(*dp && *dp != ':') dp++;
    if(*dp == ':') { dp++; while(*dp == ' ') dp++; snprintf(uid, sizeof(uid), "%.8s", dp); }
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, SAVE_DIR);
    char path[80];
    snprintf(path, sizeof(path), SAVE_DIR "/%s.txt", uid);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char ln[40];
        if(app->badge_number) {
            snprintf(ln, sizeof(ln), "# Badge: %lu\n", (unsigned long)app->badge_number);
            storage_file_write(file, ln, (uint16_t)strlen(ln));
        }
        if(app->sweep_pass_idx >= 0) {
            snprintf(ln, sizeof(ln), "# Key: %s\n", PAXTON_PASS_NAMES[app->sweep_pass_idx]);
            storage_file_write(file, ln, (uint16_t)strlen(ln));
        }
        for(int i = 0; i < app->dump_count; i++) {
            snprintf(ln, sizeof(ln), "%s\n", app->dump_lines[i]);
            storage_file_write(file, ln, (uint16_t)strlen(ln));
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void load_saved_list(App* app) {
    app->saved_count  = 0;
    app->saved_sel    = 0;
    app->saved_scroll = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, SAVE_DIR)) {
        FileInfo fi;
        char name[32];
        while(storage_dir_read(dir, &fi, name, (uint16_t)sizeof(name)) &&
              app->saved_count < SAVED_MAX) {
            if(fi.flags & FSF_DIRECTORY) continue;
            size_t nl = strlen(name);
            if(nl > 4 && strcmp(name + nl - 4, ".txt") == 0) name[nl - 4] = '\0';
            snprintf(app->saved_names[app->saved_count], 20, "%.19s", name);
            app->saved_count++;
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
}

static void load_dump_file(App* app, int idx) {
    if(idx < 0 || idx >= app->saved_count) return;
    app->dump_count     = 0;
    app->dump_scroll    = 0;
    app->badge_number   = 0;
    app->sweep_pass_idx = -1;
    app->result[0]      = '\0';
    app->result2[0]     = '\0';
    char path[80];
    snprintf(path, sizeof(path), SAVE_DIR "/%s.txt", app->saved_names[idx]);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char line[32]; int li = 0; uint8_t ch;
        while(storage_file_read(file, &ch, 1) > 0) {
            if(ch == '\n' || li >= (int)sizeof(line) - 1) {
                line[li] = '\0'; li = 0;
                if(strncmp(line, "# Badge: ", 9) == 0)
                    app->badge_number = (uint32_t)strtoul(line + 9, NULL, 10);
                else if(strncmp(line, "# Key: ", 7) == 0) {
                    snprintf(app->sweep_pass_name, sizeof(app->sweep_pass_name),
                             "%.23s", line + 7);
                    app->sweep_pass_idx = 0;
                } else if(app->dump_count < DUMP_MAX && line[0] == 'B') {
                    snprintf(app->dump_lines[app->dump_count++], 20, "%.19s", line);
                }
            } else if(ch != '\r') {
                line[li++] = (char)ch;
            }
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(app->dump_count > 0)
        app->state = StateDump;
    else {
        snprintf(app->result, sizeof(app->result), "Empty/invalid file");
        app->state = StateError;
    }
}

// Send cmd, drain all bytes until idle for `settle_ms`, return byte count.
// Logs raw printable content at INFO level.
static size_t cmd_drain(App* app, const char* cmd, char* buf, size_t max,
                        uint32_t first_byte_ms, uint32_t settle_ms) {
    flush_rx(app);
    if(cmd) serial_tx(app, cmd);

    // Wait up to first_byte_ms for the first byte to arrive
    size_t n = 0;
    uint32_t end = furi_get_tick() + furi_ms_to_ticks(first_byte_ms);
    while(furi_get_tick() < end && n == 0) {
        uint8_t b;
        if(furi_stream_buffer_receive(app->rxs, &b, 1, 10)) {
            buf[n++] = (char)b;
        }
    }
    if(n == 0) {
        buf[0] = '\0';
        return 0; // nothing came back
    }

    // Drain until no bytes arrive for settle_ms
    end = furi_get_tick() + furi_ms_to_ticks(settle_ms);
    while(n < max - 1) {
        uint8_t b;
        if(furi_stream_buffer_receive(app->rxs, &b, 1, 10)) {
            buf[n++] = (char)b;
            end = furi_get_tick() + furi_ms_to_ticks(settle_ms); // reset idle timer
        } else if(furi_get_tick() >= end) {
            break;
        }
    }
    buf[n] = '\0';

    // Log printable representation
    char pr[128]; int pi = 0;
    for(size_t i = 0; i < n && pi < 126; i++) {
        char c = buf[i];
        pr[pi++] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    pr[pi] = '\0';
    FURI_LOG_I(TAG_LOG, "cmd=[%s] n=%u resp=[%s]",
        cmd ? cmd : "(flush)", (unsigned)n, pr);
    return n;
}

static int32_t worker(void* ctx) {
    App*          app = ctx;
    const TagCfg* t   = app->cur;
    char          resp[RESP_SZ];
    char          msg[48];
    char          uid[9];

    app->sweep_pass_idx = -1;
    app->badge_number   = 0;
    app->result2[0]     = '\0';
    flush_rx(app);

    // ── Test connection ───────────────────────────────────────────────────────
    if(!t->cmd_select) {
        // Send PING and wait up to 2 s for any response
        size_t n = cmd_drain(app, "PING\r\n", resp, sizeof(resp), 2000, 200);
        if(n == 0) {
            snprintf(app->result, sizeof(app->result), "0 bytes - check TX/RX wiring");
            app->state = StateError;
        } else {
            char pr[20]; int pi = 0;
            for(size_t i = 0; i < n && pi < 18; i++) {
                char c = resp[i];
                pr[pi++] = (c >= 0x20 && c < 0x7F) ? c : '.';
            }
            pr[pi] = '\0';
            snprintf(app->result, sizeof(app->result), "%u bytes: %s", (unsigned)n, pr);
            app->state = StateDone;
        }
        view_port_update(app->vp);
        return 0;
    }

    // ── Reboot RFIDler for a clean state, then verify with PING ─────────────
    // REBOOT is the correct command (not RESET). On GPIO UART there is no
    // USB disconnect — the device just restarts. We do a fixed sleep then
    // flush the boot banner, and use PING (not bare \r\n — that dumps 12 KB
    // of help text in CLI mode) to confirm the device is ready.
    serial_tx(app, "REBOOT\r\n");
    furi_delay_ms(3000);
    flush_rx(app);
    {
        size_t n = cmd_drain(app, "PING\r\n", resp, sizeof(resp), 2000, 300);
        if(n == 0) {
            snprintf(app->result, sizeof(app->result), "No response - check wiring");
            app->state = StateError;
            view_port_update(app->vp);
            return 0;
        }
    }

    // ── 1. Select tag type ───────────────────────────────────────────────────
    // API mode: success = '+' or '.', error = '!' prefix.
    size_t sel_n = cmd_drain(app, t->cmd_select, resp, sizeof(resp), 2000, 300);
    if(sel_n == 0 || resp[0] == '!') {
        extract_msg(resp, msg, sizeof(msg));
        snprintf(app->result, sizeof(app->result),
            sel_n ? msg[0] ? msg : "Set tag err" : "No resp to SET TAG");
        app->state = StateError;
        view_port_update(app->vp);
        return 0;
    }

    // ── 2. Dump: SAVE → LOGIN → DUMP 0 7 ────────────────────────────────────
    // Matches the working terminal flow exactly.
    // SAVE (already sent in step 1b) initialises vtag before LOGIN.
    // DUMP 0 7 reads live from the card; the RFIDler prints a coil-sync
    // banner before the page data, so use a long settle time (2 s) to
    // capture the data that follows the banner.
    if(t->is_dump) {
        app->dump_count  = 0;
        app->dump_scroll = 0;

        // API mode: '!' prefix = error, '+'/'.'' = success.
        // Login fail response: "!Login failed!\r\n" — contains "fail".
        // DUMP response:       "+0: 0049E513\r\n...\r\n*\r\n" — fast, no coil banner.
        if(t->sweep_pass) {
            for(int pi = 0; pi < PAXTON_N_PASS; pi++) {
                if(pi > 0) {
                    cmd_drain(app, t->cmd_select, resp, sizeof(resp), 2000, 300);
                    cmd_drain(app, "SAVE\r\n",       resp, sizeof(resp), 2000, 300);
                }
                char login_cmd[32];
                snprintf(login_cmd, sizeof(login_cmd), "LOGIN %s\r\n", PAXTON_PASSWORDS[pi]);
                size_t ln = cmd_drain(app, login_cmd, resp, sizeof(resp), 4000, 300);
                if(ln == 0 || resp[0] == '!' || ihas(resp, "fail"))
                    continue;
                size_t dn = cmd_drain(app, "DUMP 0 7\r\n", resp, sizeof(resp), 5000, 500);
                int cnt = (dn > 0) ? parse_dump(resp, app->dump_lines, DUMP_MAX) : 0;
                if(cnt > 0) {
                    app->dump_count     = cnt;
                    app->sweep_pass_idx = pi;
                    snprintf(app->sweep_pass_name, sizeof(app->sweep_pass_name),
                        "%s", PAXTON_PASS_NAMES[pi]);
                    FURI_LOG_I(TAG_LOG, "Paxton dump OK with %s", PAXTON_PASS_NAMES[pi]);
                    break;
                }
            }
            if(app->dump_count == 0) {
                snprintf(app->result, sizeof(app->result), "All passwords failed");
                app->state = StateError;
                view_port_update(app->vp);
                return 0;
            }
        } else {
            // Fixed auth: LOGIN → DUMP 0 7
            size_t ln = cmd_drain(app, t->cmd_auth, resp, sizeof(resp), 4000, 300);
            if(ln == 0 || resp[0] == '!' || ihas(resp, "fail")) {
                extract_msg(resp, msg, sizeof(msg));
                snprintf(app->result, sizeof(app->result),
                    ln ? msg[0] ? msg : "Auth failed" : "Auth timeout");
                app->state = StateError;
                view_port_update(app->vp);
                return 0;
            }
            size_t dn = cmd_drain(app, t->cmd_read, resp, sizeof(resp), 5000, 500);
            app->dump_count = (dn > 0) ? parse_dump(resp, app->dump_lines, DUMP_MAX) : 0;
            if(app->dump_count == 0) {
                char pr[48]; int pj = 0;
                for(size_t i = 0; i < dn && pj < 46; i++) {
                    char c = resp[i];
                    pr[pj++] = (c >= 0x20 && c < 0x7F) ? c : '.';
                }
                pr[pj] = '\0';
                snprintf(app->result,  sizeof(app->result),  dn ? "No blocks:" : "No response");
                snprintf(app->result2, sizeof(app->result2), "%s", pr);
                app->state = StateError;
                view_port_update(app->vp);
                return 0;
            }
        }

        app->badge_number = decode_badge(
            (const char(*)[20])app->dump_lines, app->dump_count);
        save_dump(app);
        app->state = StateDump;
        if(app->sweep_pass_idx >= 0)
            snprintf(app->result, sizeof(app->result), "Key: %s", app->sweep_pass_name);
        else
            snprintf(app->result, sizeof(app->result), "Dump OK");
        view_port_update(app->vp);
        return 0;
    }

    // ── 3. Auth for non-dump entries ──────────────────────────────────────────
    if(t->cmd_auth) {
        size_t n = cmd_drain(app, t->cmd_auth, resp, sizeof(resp), 4000, 300);
        if(n == 0 || resp[0] == '!' || ihas(resp, "fail")) {
            extract_msg(resp, msg, sizeof(msg));
            snprintf(app->result, sizeof(app->result),
                n ? msg[0] ? msg : "Auth failed" : "Auth no response");
            app->state = StateError;
            view_port_update(app->vp);
            return 0;
        }
    }

    // ── 3b. UID ───────────────────────────────────────────────────────────────
    size_t n = cmd_drain(app, t->cmd_read, resp, sizeof(resp), 3000, 300);
    if(n > 0 && parse_uid(resp, uid)) {
        snprintf(app->result, sizeof(app->result), "UID: %s", uid);
        app->state = StateDone;
    } else {
        extract_msg(resp, msg, sizeof(msg));
        snprintf(app->result, sizeof(app->result),
            n ? msg[0] ? msg : "No tag / no UID" : "No response");
        app->state = StateError;
    }

    view_port_update(app->vp);
    return 0;
}

// ── Draw ──────────────────────────────────────────────────────────────────────

static void draw_idle(Canvas* c, App* app) {
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 0, 9, "RFIDler  pin13=TX  pin14=RX");
    canvas_draw_line(c, 0, 11, 127, 11);

    for(int i = 0; i < MENU_VISIBLE; i++) {
        int idx = app->menu_scroll + i;
        if(idx >= TAG_COUNT) break;
        int y = LIST_TOP + i * ITEM_H;
        if(idx == app->sel) {
            canvas_draw_box(c, 0, y, 128, ITEM_H);
            canvas_set_color(c, ColorWhite);
            canvas_draw_str(c, 3, y + ITEM_H - 2, TAGS[idx].name);
            canvas_set_color(c, ColorBlack);
        } else {
            canvas_draw_str(c, 3, y + ITEM_H - 2, TAGS[idx].name);
        }
    }

    // Scroll arrows — only when needed, placed inside list area
    if(app->menu_scroll > 0)
        canvas_draw_str(c, 122, LIST_TOP + ITEM_H - 2, "^");
    if(app->menu_scroll + MENU_VISIBLE < TAG_COUNT)
        canvas_draw_str(c, 122, LIST_TOP + (MENU_VISIBLE - 1) * ITEM_H + ITEM_H - 2, "v");

    canvas_draw_line(c, 0, 52, 127, 52);
    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "[OK] Read   [Back] Quit");
}

static void draw_result(Canvas* c, App* app) {
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 0, 9, TAGS[app->sel].name);
    canvas_draw_line(c, 0, 11, 127, 11);

    if(app->state == StateBusy) {
        canvas_draw_str_aligned(c, 64, 36, AlignCenter, AlignCenter, "Querying...");
        return;
    }
    if(app->state == StateDone) {
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(c, 64, 34, AlignCenter, AlignCenter, app->result);
    } else if(app->result2[0]) {
        // Two-line error: title + raw response detail
        canvas_draw_str_aligned(c, 64, 22, AlignCenter, AlignCenter, "Error:");
        canvas_draw_str_aligned(c, 64, 32, AlignCenter, AlignCenter, app->result);
        canvas_draw_str(c, 0, 44, app->result2);          // left-aligned so it's not clipped
        canvas_draw_str(c, 0, 52, app->result2 + 21);     // second half if long
    } else {
        canvas_draw_str_aligned(c, 64, 26, AlignCenter, AlignCenter, "Error:");
        canvas_draw_str_aligned(c, 64, 40, AlignCenter, AlignCenter, app->result);
    }
    canvas_set_font(c, FontSecondary);
    canvas_draw_line(c, 0, 55, 127, 55);
    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "[OK] Retry  [Back] Menu");
}

static void draw_dump(Canvas* c, App* app) {
    canvas_set_font(c, FontSecondary);

    // Header: tag name + which password succeeded
    char hdr[36];
    if(app->sweep_pass_idx >= 0)
        snprintf(hdr, sizeof(hdr), "Paxton [%.23s]", app->sweep_pass_name);
    else
        snprintf(hdr, sizeof(hdr), "Paxton Dump");
    canvas_draw_str(c, 0, 9, hdr);
    canvas_draw_line(c, 0, 11, 127, 11);

    // 4 rows (y=21,29,37,45) leaving room for badge at y=55 and footer at y=63
    for(int i = 0; i < DUMP_VISIBLE; i++) {
        int idx = app->dump_scroll + i;
        if(idx >= app->dump_count) break;
        canvas_draw_str(c, 2, 21 + i * 8, app->dump_lines[idx]);
    }

    if(app->dump_scroll > 0)
        canvas_draw_str(c, 122, 21, "^");
    if(app->dump_scroll + DUMP_VISIBLE < app->dump_count)
        canvas_draw_str(c, 122, 45, "v");

    // Badge number on its own row, clear of the block list
    if(app->badge_number) {
        char bstr[22];
        snprintf(bstr, sizeof(bstr), "Badge: %lu", (unsigned long)app->badge_number);
        canvas_draw_str(c, 0, 55, bstr);
    }

    canvas_draw_line(c, 0, 56, 127, 56);
    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "[Up/Dn] Scroll  [Back] Menu");
}

static void draw_saved_list(Canvas* c, App* app) {
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 0, 9, "Saved Dumps");
    canvas_draw_line(c, 0, 11, 127, 11);

    if(app->saved_count == 0) {
        canvas_draw_str_aligned(c, 64, 36, AlignCenter, AlignCenter, "No saved dumps");
    } else {
        for(int i = 0; i < SAVED_VISIBLE; i++) {
            int idx = app->saved_scroll + i;
            if(idx >= app->saved_count) break;
            int y = LIST_TOP + i * ITEM_H;
            if(idx == app->saved_sel) {
                canvas_draw_box(c, 0, y, 128, ITEM_H);
                canvas_set_color(c, ColorWhite);
                canvas_draw_str(c, 3, y + ITEM_H - 2, app->saved_names[idx]);
                canvas_set_color(c, ColorBlack);
            } else {
                canvas_draw_str(c, 3, y + ITEM_H - 2, app->saved_names[idx]);
            }
        }
        if(app->saved_scroll > 0)
            canvas_draw_str(c, 122, LIST_TOP + ITEM_H - 2, "^");
        if(app->saved_scroll + SAVED_VISIBLE < app->saved_count)
            canvas_draw_str(c, 122, LIST_TOP + (SAVED_VISIBLE - 1) * ITEM_H + ITEM_H - 2, "v");
    }

    canvas_draw_line(c, 0, 52, 127, 52);
    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "[OK] View  [Back] Menu");
}

static void draw_cb(Canvas* c, void* ctx) {
    App* app = ctx;
    canvas_clear(c);
    switch(app->state) {
    case StateIdle:       draw_idle(c, app);       break;
    case StateDump:       draw_dump(c, app);       break;
    case StateSavedList:  draw_saved_list(c, app); break;
    default:              draw_result(c, app);     break;
    }
}

static void input_cb(InputEvent* ev, void* ctx) {
    furi_message_queue_put(((App*)ctx)->eq, ev, 0);
}

// ── Entry point ───────────────────────────────────────────────────────────────

static void start_read(App* app) {
    if(TAGS[app->sel].is_file_browser) {
        load_saved_list(app);
        app->state = StateSavedList;
        view_port_update(app->vp);
        return;
    }
    if(app->worker) {
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
    app->cur   = &TAGS[app->sel];
    app->state = StateBusy;
    view_port_update(app->vp);
    app->worker = furi_thread_alloc_ex("rfidler_w", 2048, worker, app);
    furi_thread_start(app->worker);
}

static void menu_move(App* app, int dir) {
    app->sel = (app->sel + dir + TAG_COUNT) % TAG_COUNT;
    // Keep selection inside the visible window
    if(app->sel < app->menu_scroll)
        app->menu_scroll = app->sel;
    else if(app->sel >= app->menu_scroll + MENU_VISIBLE)
        app->menu_scroll = app->sel - MENU_VISIBLE + 1;
    // Handle wrap-around jump
    if(dir > 0 && app->sel == 0) app->menu_scroll = 0;
    if(dir < 0 && app->sel == TAG_COUNT - 1)
        app->menu_scroll = TAG_COUNT - MENU_VISIBLE;
    view_port_update(app->vp);
}

int32_t rfidler_app_main(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->state          = StateIdle;
    app->sweep_pass_idx = -1;
    app->rxs = furi_stream_buffer_alloc(RX_BUF_SZ, 1);

    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_assert(app->serial);
    furi_hal_serial_init(app->serial, UART_BAUD);
    furi_hal_serial_async_rx_start(app->serial, serial_rx_cb, app, false);

    app->eq  = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->vp  = view_port_alloc();
    app->gui = furi_record_open(RECORD_GUI);
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    InputEvent ev;
    bool       running = true;
    while(running && furi_message_queue_get(app->eq, &ev, FuriWaitForever) == FuriStatusOk) {
        if(ev.type != InputTypeShort && ev.type != InputTypeRepeat) continue;
        switch(ev.key) {
        case InputKeyBack:
            if(app->state == StateIdle) running = false;
            else if(app->state != StateBusy) { app->state = StateIdle; view_port_update(app->vp); }
            break;
        case InputKeyUp:
            if(app->state == StateIdle)
                menu_move(app, -1);
            else if(app->state == StateDump && app->dump_scroll > 0)
                { app->dump_scroll--; view_port_update(app->vp); }
            else if(app->state == StateSavedList && app->saved_sel > 0) {
                app->saved_sel--;
                if(app->saved_sel < app->saved_scroll) app->saved_scroll = app->saved_sel;
                view_port_update(app->vp);
            }
            break;
        case InputKeyDown:
            if(app->state == StateIdle)
                menu_move(app, +1);
            else if(app->state == StateDump && app->dump_scroll + DUMP_VISIBLE < app->dump_count)
                { app->dump_scroll++; view_port_update(app->vp); }
            else if(app->state == StateSavedList && app->saved_sel < app->saved_count - 1) {
                app->saved_sel++;
                if(app->saved_sel >= app->saved_scroll + SAVED_VISIBLE)
                    app->saved_scroll = app->saved_sel - SAVED_VISIBLE + 1;
                view_port_update(app->vp);
            }
            break;
        case InputKeyOk:
            if(app->state == StateSavedList) {
                load_dump_file(app, app->saved_sel);
                view_port_update(app->vp);
            } else if(app->state != StateBusy && app->state != StateDump) {
                start_read(app);
            }
            break;
        default: break;
        }
    }

    if(app->worker) { furi_thread_join(app->worker); furi_thread_free(app->worker); }
    furi_hal_serial_async_rx_stop(app->serial);
    furi_hal_serial_deinit(app->serial);
    furi_hal_serial_control_release(app->serial);
    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->eq);
    furi_stream_buffer_free(app->rxs);
    free(app);
    return 0;
}
