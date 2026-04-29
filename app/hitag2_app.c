#include <furi.h>
#include <string.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include "hitag2_passwords.h"
#include "hitag2_reader.h"
#include "hitag2_crypto.h"

// ── View IDs ─────────────────────────────────────────────────────────────────

typedef enum {
    ViewMenu = 0,
    ViewRead,
    ViewDump,
    ViewCrack,
    ViewAbout,
} AppView;

typedef enum {
    MenuRead = 0,
    MenuDump,
    MenuCrack,
    MenuAbout,
} MenuItem;

typedef enum {
    EventReadOk = 0,
    EventReadFail,
    EventDumpOk,
    EventDumpFail,
    EventCrackOk,
    EventCrackFail,
    EventCrackProgress,
} AppEvent;

// ── Models ────────────────────────────────────────────────────────────────────

typedef struct {
    char     status[48];
    char     sub[32];
    uint32_t uid;
    bool     found;
} ReadModel;

typedef struct {
    char     status[48];
    char     sub[32];
    uint32_t pages[8];
    int      n_pages;
    uint32_t nonce;
    int      scroll;     // display scroll offset
    bool     have_dump;
    uint32_t badge;      // decoded Paxton badge number
    uint32_t password;   // password that worked (0 = unknown)
    char     pass_name[20];
} DumpModel;

typedef struct {
    char     status[48];
    char     sub[32];
    int      pairs_collected;
    bool     cracking;
    bool     found;
    uint64_t key48;
    uint32_t uid;
    uint32_t nonce_buf[32]; // collected nonces for crack
    uint32_t resp_buf[32];  // corresponding tag responses
} CrackModel;

// ── App struct ────────────────────────────────────────────────────────────────

typedef struct {
    ViewDispatcher*  view_dispatcher;
    Submenu*         submenu;
    View*            read_view;
    View*            dump_view;
    View*            crack_view;
    View*            about_view;
    NotificationApp* notifications;
    FuriThread*      worker_thread;
    volatile bool    worker_stop;
    volatile bool    worker_running;
} HiTag2App;

// ── Draw callbacks ────────────────────────────────────────────────────────────

static void read_view_draw(Canvas* canvas, void* model_ptr) {
    ReadModel* m = model_ptr;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "HiTag2 Reader");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignTop, "Hold tag to back of Flipper");
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, m->status);
    if(m->found) {
        char uid_buf[24];
        snprintf(uid_buf, sizeof(uid_buf), "UID: %08lX", (unsigned long)m->uid);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignTop, uid_buf);
    } else if(m->sub[0]) {
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignTop, m->sub);
    }
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "[Back] stop");
}

static void dump_view_draw(Canvas* canvas, void* model_ptr) {
    DumpModel* m = model_ptr;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Paxton Dump");
    canvas_set_font(canvas, FontSecondary);

    if(!m->have_dump) {
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, m->status);
        if(m->sub[0])
            canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignTop, m->sub);
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "[Back] stop");
        return;
    }

    // Scrollable page dump
    int visible = 4;
    for(int i = 0; i < visible; i++) {
        int pg = m->scroll + i;
        if(pg >= m->n_pages) break;
        char buf[28];
        snprintf(buf, sizeof(buf), "P%d: %08lX", pg, (unsigned long)m->pages[pg]);
        canvas_draw_str(canvas, 2, 14 + i * 10, buf);
    }

    // Badge number if available
    if(m->badge) {
        char bstr[20];
        snprintf(bstr, sizeof(bstr), "Badge: %lu", (unsigned long)m->badge);
        canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignTop, bstr);
    }

    if(m->scroll > 0) canvas_draw_str(canvas, 120, 14, "^");
    if(m->scroll + visible < m->n_pages) canvas_draw_str(canvas, 120, 52, "v");

    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom,
        "[Up/Dn] Scroll  [Back] Menu");
}

static void crack_view_draw(Canvas* canvas, void* model_ptr) {
    CrackModel* m = model_ptr;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "HiTag2 Crack");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignTop, m->status);

    if(m->found) {
        char kbuf[22];
        snprintf(kbuf, sizeof(kbuf), "%06llX%06llX",
            (unsigned long long)((m->key48 >> 24) & 0xFFFFFFULL),
            (unsigned long long)(m->key48 & 0xFFFFFFULL));
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, "Key found:");
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignTop, kbuf);
    } else {
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, m->sub);
        char pbuf[24];
        snprintf(pbuf, sizeof(pbuf), "Pairs: %d/8", m->pairs_collected);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignTop, pbuf);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "[Back] stop");
}

static void about_view_draw(Canvas* canvas, void* model_ptr) {
    (void)model_ptr;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "HiTag2");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignTop, "NXP PCF7936 125 kHz RFID");
    canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignTop, "Crack: Sheldrake / Verdult");
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignTop, "HW fix: 15-22R series LF");
    canvas_draw_str_aligned(canvas, 64, 46, AlignCenter, AlignTop, "v0.2 - Craig");
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "[Back]");
}

static bool view_input_stub(InputEvent* event, void* ctx) {
    (void)event; (void)ctx;
    return false;
}

static bool dump_view_input(InputEvent* event, void* ctx_ptr) {
    HiTag2App* app = ctx_ptr;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool handled = false;
    with_view_model(app->dump_view, DumpModel* m, {
        if(m->have_dump) {
            if(event->key == InputKeyUp && m->scroll > 0) {
                m->scroll--;
                handled = true;
            } else if(event->key == InputKeyDown && m->scroll + 4 < m->n_pages) {
                m->scroll++;
                handled = true;
            }
        }
    }, handled);
    return handled;
}

// ── Worker threads ────────────────────────────────────────────────────────────

static int32_t reader_thread_fn(void* ctx_ptr) {
    HiTag2App* app = ctx_ptr;
    app->worker_running = true;

    uint32_t uid = 0;
    bool     ok  = false;
    int      attempt = 0;

    while(!app->worker_stop) {
        attempt++;
        with_view_model(app->read_view, ReadModel* m, {
            snprintf(m->status, sizeof(m->status), "Sending START_AUTH...");
            snprintf(m->sub,    sizeof(m->sub),    "Attempt %d", attempt);
            m->found = false;
        }, true);

        int bits = 0, comp = 0, bplm = 0;
        ok = hitag2_hw_read_uid(&uid, &bits, &comp, &bplm, 600);

        if(ok) break;

        with_view_model(app->read_view, ReadModel* m, {
            if(bits > 0)
                snprintf(m->status, sizeof(m->status), "Got %d/32 bits", bits);
            else if(comp > 0)
                snprintf(m->status, sizeof(m->status), "Comp:%d no bits", comp);
            else
                snprintf(m->status, sizeof(m->status), "No response");
            snprintf(m->sub, sizeof(m->sub), "b=%d c=%d #%d", bplm, comp, attempt);
            m->found = false;
        }, true);

        furi_delay_ms(1500);
    }

    if(ok && !app->worker_stop) {
        with_view_model(app->read_view, ReadModel* m, {
            snprintf(m->status, sizeof(m->status), "Tag found!");
            m->sub[0] = '\0';
            m->uid    = uid;
            m->found  = true;
        }, true);
        view_dispatcher_send_custom_event(app->view_dispatcher, EventReadOk);
    } else if(!app->worker_stop) {
        view_dispatcher_send_custom_event(app->view_dispatcher, EventReadFail);
    }

    app->worker_running = false;
    return 0;
}

static int32_t dump_thread_fn(void* ctx_ptr) {
    HiTag2App* app = ctx_ptr;
    app->worker_running = true;

    uint32_t pages[8] = {0};
    uint32_t nonce = 0;
    int      comp  = 0;
    bool     ok    = false;

    // Try each known password in order
    for(int pi = 0; pi < HITAG2_PASSWORD_COUNT && !app->worker_stop && !ok; pi++) {
        uint32_t pw = hitag2_default_passwords[pi];

        with_view_model(app->dump_view, DumpModel* m, {
            snprintf(m->status, sizeof(m->status), "Trying %s", hitag2_password_names[pi]);
            snprintf(m->sub,    sizeof(m->sub),    "%08lX", (unsigned long)pw);
            m->have_dump = false;
        }, true);

        ok = hitag2_hw_dump(pw, pages, 8, &nonce, &comp, 800);

        if(ok) {
            uint32_t badge = paxton_badge_number(pages[2]);
            with_view_model(app->dump_view, DumpModel* m, {
                memcpy(m->pages, pages, sizeof(pages));
                m->n_pages  = 8;
                m->nonce    = nonce;
                m->scroll   = 0;
                m->badge    = badge;
                m->password = pw;
                m->have_dump = true;
                snprintf(m->status, sizeof(m->status), "Dumped!");
                snprintf(m->pass_name, sizeof(m->pass_name), "%s", hitag2_password_names[pi]);
                m->sub[0] = '\0';
            }, true);

            FURI_LOG_I("Dump", "Success with '%s' (0x%08lX) nonce=0x%08lX badge=%lu",
                hitag2_password_names[pi], (unsigned long)pw,
                (unsigned long)nonce, (unsigned long)badge);

            // Save dump to SD card
            Storage* storage = furi_record_open(RECORD_STORAGE);
            File*    file    = storage_file_alloc(storage);
            if(storage_file_open(file, "/ext/hitag2_dump.txt",
                                 FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                char line[48];
                snprintf(line, sizeof(line), "Password: %s\n", hitag2_password_names[pi]);
                storage_file_write(file, line, strlen(line));
                snprintf(line, sizeof(line), "Nonce: %08lX\n", (unsigned long)nonce);
                storage_file_write(file, line, strlen(line));
                for(int pg = 0; pg < 8; pg++) {
                    snprintf(line, sizeof(line), "Page%d: %08lX\n",
                        pg, (unsigned long)pages[pg]);
                    storage_file_write(file, line, strlen(line));
                }
                if(badge) {
                    snprintf(line, sizeof(line), "Badge: %lu\n", (unsigned long)badge);
                    storage_file_write(file, line, strlen(line));
                }
                storage_file_close(file);
            }
            storage_file_free(file);
            furi_record_close(RECORD_STORAGE);

            view_dispatcher_send_custom_event(app->view_dispatcher, EventDumpOk);
            break;
        }

        furi_delay_ms(200);
    }

    if(!ok && !app->worker_stop) {
        with_view_model(app->dump_view, DumpModel* m, {
            snprintf(m->status, sizeof(m->status), "All passwords failed");
            snprintf(m->sub,    sizeof(m->sub),    "comp=%d", comp);
            m->have_dump = false;
        }, true);
        view_dispatcher_send_custom_event(app->view_dispatcher, EventDumpFail);
    }

    app->worker_running = false;
    return 0;
}

static int32_t crack_thread_fn(void* ctx_ptr) {
    HiTag2App* app = ctx_ptr;
    app->worker_running = true;

    // Phase 1: collect up to 8 (nonce, response) pairs by repeating START_AUTH
    // In crypto mode the tag sends its response encrypted — we capture both.
    // In public/password mode the "response" is just the UID from page 0.
    uint32_t nonces[8] = {0};
    uint32_t resps[8]  = {0};
    int      n_pairs   = 0;
    uint32_t uid       = 0;

    with_view_model(app->crack_view, CrackModel* m, {
        snprintf(m->status, sizeof(m->status), "Collecting nonces...");
        snprintf(m->sub,    sizeof(m->sub),    "Hold tag steady");
        m->pairs_collected = 0;
        m->found = false;
    }, true);

    for(int i = 0; i < 8 && !app->worker_stop; i++) {
        uint32_t nonce = 0;
        if(!hitag2_hw_collect_nonce(&nonce, 500)) {
            furi_delay_ms(100);
            continue;
        }
        nonces[n_pairs] = nonce;
        // For now use 0 as the "response" — real crypto responses need the
        // reader to authenticate first. TODO: extend once full crypto mode
        // reader send is wired up.
        resps[n_pairs] = 0;
        if(n_pairs == 0) uid = nonce; // first nonce used as UID placeholder
        n_pairs++;

        with_view_model(app->crack_view, CrackModel* m, {
            m->pairs_collected = n_pairs;
            snprintf(m->status, sizeof(m->status), "Collected %d nonce(s)", n_pairs);
        }, true);

        furi_delay_ms(150);
    }

    if(n_pairs == 0 || app->worker_stop) {
        with_view_model(app->crack_view, CrackModel* m, {
            snprintf(m->status, sizeof(m->status), "No response from tag");
            m->sub[0] = '\0';
        }, true);
        view_dispatcher_send_custom_event(app->view_dispatcher, EventCrackFail);
        app->worker_running = false;
        return 0;
    }

    // Phase 2: dictionary attack using known passwords
    with_view_model(app->crack_view, CrackModel* m, {
        snprintf(m->status, sizeof(m->status), "Dict attack...");
        m->uid = uid;
    }, true);

    uint64_t found_key = 0;
    int dict_idx = hitag2_dict_crack(uid, nonces[0], resps[0],
                                     hitag2_default_passwords,
                                     HITAG2_PASSWORD_COUNT, &found_key);

    if(dict_idx >= 0) {
        with_view_model(app->crack_view, CrackModel* m, {
            snprintf(m->status, sizeof(m->status), "Dict match: %s",
                hitag2_password_names[dict_idx]);
            m->found  = true;
            m->key48  = found_key;
        }, true);
        view_dispatcher_send_custom_event(app->view_dispatcher, EventCrackOk);
        app->worker_running = false;
        return 0;
    }

    // Phase 3: fast online crack (upper-16-bit sweep)
    with_view_model(app->crack_view, CrackModel* m, {
        snprintf(m->status, sizeof(m->status), "Fast crack (16-bit)...");
        snprintf(m->sub,    sizeof(m->sub),    "~2-4 sec on Flipper");
    }, true);

    Hitag2Pair pairs[8];
    for(int i = 0; i < n_pairs; i++) {
        pairs[i].tag_nonce    = nonces[i];
        pairs[i].tag_response = resps[i];
    }

    bool cracked = hitag2_fast_crack(uid, pairs, n_pairs, &found_key);

    if(cracked) {
        with_view_model(app->crack_view, CrackModel* m, {
            snprintf(m->status, sizeof(m->status), "Key recovered!");
            m->found  = true;
            m->key48  = found_key;
        }, true);
        view_dispatcher_send_custom_event(app->view_dispatcher, EventCrackOk);
    } else {
        with_view_model(app->crack_view, CrackModel* m, {
            snprintf(m->status, sizeof(m->status), "Crack failed");
            snprintf(m->sub,    sizeof(m->sub),    "Need crypto responses");
        }, true);
        view_dispatcher_send_custom_event(app->view_dispatcher, EventCrackFail);
    }

    app->worker_running = false;
    return 0;
}

// ── Custom event handler ──────────────────────────────────────────────────────

static bool custom_event_handler(void* ctx, uint32_t event) {
    HiTag2App* app = ctx;
    switch((AppEvent)event) {
    case EventReadOk:
    case EventDumpOk:
    case EventCrackOk:
        notification_message(app->notifications, &sequence_success);
        break;
    case EventReadFail:
    case EventDumpFail:
    case EventCrackFail:
        notification_message(app->notifications, &sequence_error);
        break;
    default:
        break;
    }
    return true;
}

// ── Thread management helpers ─────────────────────────────────────────────────

static void stop_worker(HiTag2App* app) {
    if(app->worker_thread) {
        app->worker_stop = true;
        furi_thread_join(app->worker_thread);
        furi_thread_free(app->worker_thread);
        app->worker_thread = NULL;
    }
    app->worker_stop = false;
}

static void start_worker(HiTag2App* app, FuriThreadCallback fn, size_t stack) {
    stop_worker(app);
    app->worker_thread = furi_thread_alloc_ex("HT2Worker", stack, fn, app);
    furi_thread_start(app->worker_thread);
}

// ── View enter / exit ─────────────────────────────────────────────────────────

static void on_read_enter(void* ctx) {
    HiTag2App* app = ctx;
    with_view_model(app->read_view, ReadModel* m, {
        snprintf(m->status, sizeof(m->status), "Starting...");
        m->sub[0] = '\0';
        m->found  = false;
        m->uid    = 0;
    }, true);
    start_worker(app, reader_thread_fn, 2048);
}

static void on_read_exit(void* ctx) {
    stop_worker((HiTag2App*)ctx);
}

static void on_dump_enter(void* ctx) {
    HiTag2App* app = ctx;
    with_view_model(app->dump_view, DumpModel* m, {
        snprintf(m->status, sizeof(m->status), "Starting...");
        m->sub[0]    = '\0';
        m->have_dump = false;
        m->scroll    = 0;
        m->badge     = 0;
    }, true);
    start_worker(app, dump_thread_fn, 3072);
}

static void on_dump_exit(void* ctx) {
    stop_worker((HiTag2App*)ctx);
}

static void on_crack_enter(void* ctx) {
    HiTag2App* app = ctx;
    with_view_model(app->crack_view, CrackModel* m, {
        snprintf(m->status, sizeof(m->status), "Starting...");
        m->sub[0]          = '\0';
        m->pairs_collected = 0;
        m->found           = false;
        m->key48           = 0;
    }, true);
    start_worker(app, crack_thread_fn, 3072);
}

static void on_crack_exit(void* ctx) {
    stop_worker((HiTag2App*)ctx);
}

// ── Navigation ────────────────────────────────────────────────────────────────

static void submenu_callback(void* ctx, uint32_t index) {
    HiTag2App* app = ctx;
    switch((MenuItem)index) {
    case MenuRead:  view_dispatcher_switch_to_view(app->view_dispatcher, ViewRead);  break;
    case MenuDump:  view_dispatcher_switch_to_view(app->view_dispatcher, ViewDump);  break;
    case MenuCrack: view_dispatcher_switch_to_view(app->view_dispatcher, ViewCrack); break;
    case MenuAbout: view_dispatcher_switch_to_view(app->view_dispatcher, ViewAbout); break;
    }
}

static uint32_t back_to_menu(void* ctx) { (void)ctx; return ViewMenu; }
static uint32_t nav_exit(void* ctx)     { (void)ctx; return VIEW_NONE; }

// ── App lifecycle ─────────────────────────────────────────────────────────────

static HiTag2App* hitag2_app_alloc(void) {
    HiTag2App* app = malloc(sizeof(HiTag2App));
    memset(app, 0, sizeof(*app));

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    Gui* gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_handler);

    // Menu
    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Read UID",       MenuRead,  submenu_callback, app);
    submenu_add_item(app->submenu, "Dump (Paxton)",  MenuDump,  submenu_callback, app);
    submenu_add_item(app->submenu, "Crack Key",      MenuCrack, submenu_callback, app);
    submenu_add_item(app->submenu, "About",          MenuAbout, submenu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), nav_exit);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->submenu));

    // Read view
    app->read_view = view_alloc();
    view_allocate_model(app->read_view, ViewModelTypeLocking, sizeof(ReadModel));
    view_set_draw_callback(app->read_view, read_view_draw);
    view_set_input_callback(app->read_view, view_input_stub);
    view_set_context(app->read_view, app);
    view_set_enter_callback(app->read_view, on_read_enter);
    view_set_exit_callback(app->read_view, on_read_exit);
    view_set_previous_callback(app->read_view, back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewRead, app->read_view);

    // Dump view
    app->dump_view = view_alloc();
    view_allocate_model(app->dump_view, ViewModelTypeLocking, sizeof(DumpModel));
    view_set_draw_callback(app->dump_view, dump_view_draw);
    view_set_input_callback(app->dump_view, dump_view_input);
    view_set_context(app->dump_view, app);
    view_set_enter_callback(app->dump_view, on_dump_enter);
    view_set_exit_callback(app->dump_view, on_dump_exit);
    view_set_previous_callback(app->dump_view, back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewDump, app->dump_view);

    // Crack view
    app->crack_view = view_alloc();
    view_allocate_model(app->crack_view, ViewModelTypeLocking, sizeof(CrackModel));
    view_set_draw_callback(app->crack_view, crack_view_draw);
    view_set_input_callback(app->crack_view, view_input_stub);
    view_set_context(app->crack_view, app);
    view_set_enter_callback(app->crack_view, on_crack_enter);
    view_set_exit_callback(app->crack_view, on_crack_exit);
    view_set_previous_callback(app->crack_view, back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewCrack, app->crack_view);

    // About view
    app->about_view = view_alloc();
    view_set_draw_callback(app->about_view, about_view_draw);
    view_set_input_callback(app->about_view, view_input_stub);
    view_set_previous_callback(app->about_view, back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewAbout, app->about_view);

    return app;
}

static void hitag2_app_free(HiTag2App* app) {
    stop_worker(app);

    view_dispatcher_remove_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewRead);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDump);
    view_dispatcher_remove_view(app->view_dispatcher, ViewCrack);
    view_dispatcher_remove_view(app->view_dispatcher, ViewAbout);

    submenu_free(app->submenu);
    view_free(app->read_view);
    view_free(app->dump_view);
    view_free(app->crack_view);
    view_free(app->about_view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t hitag2_app_main(void* p) {
    (void)p;
    HiTag2App* app = hitag2_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    hitag2_app_free(app);
    return 0;
}
