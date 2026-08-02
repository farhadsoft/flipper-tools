/*
 * Universal Card Reader for Flipper Zero
 * ------------------------------------------------------------
 * Reads both NFC (13.56 MHz) and LF RFID (125 kHz) cards. The two radios
 * cannot run at the same time, so the app alternates timed phases: an NFC
 * phase, then an LF phase, looping until a card is found.
 *
 * All NFC/LF start/stop calls happen on the GUI thread. Worker callbacks and
 * the phase timer only signal via view_dispatcher_send_custom_event().
 *
 * Legitimate use only: read cards/tags you own or are authorised to test.
 *
 * Target: Flipper Zero official firmware 1.x. Build with ufbt.
 */

#include <furi.h>
#include <math.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_box.h>
#include <gui/modules/submenu.h>
#include <input/input.h>

#include <nfc/nfc.h>
#include <nfc/nfc_scanner.h>
#include <nfc/nfc_poller.h>
#include <nfc/nfc_listener.h>
#include <nfc/nfc_device.h>
#include <nfc/protocols/nfc_protocol.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>
#include <nfc/protocols/iso14443_3b/iso14443_3b_poller.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>
#include <nfc/protocols/felica/felica_poller.h>
#include <nfc/protocols/st25tb/st25tb_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_classic/mf_classic_poller.h>

#include <lfrfid/lfrfid_worker.h>
#include <lfrfid/lfrfid_dict_file.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>
#include <storage/storage.h>
#include "emv.h"
#include "card_info.h"

#define TAG "UniCardReader"

#define ID_MAX_LEN     16
#define NFC_PHASE_MS   1200
#define LF_PHASE_MS    1600
#define ANIM_PERIOD_MS 80
#define NOTICE_MS      1600 // save result / blocked-action message dwell time

#define READ_TIMEOUT_MS 2500
#define EMV_READ_TIMEOUT_MS 6000

/*
 * Worker threads and the phase timer keep running for a short while after the
 * GUI thread has torn their phase down, so their events can still be sitting
 * in the dispatcher queue. Acting on such a stale event is fatal: a second
 * nfc_poller_start() on the same Nfc instance reaches nfc_start(), which
 * furi_check()s that the instance is idle. Every event therefore carries the
 * generation it was produced in, and the GUI thread drops anything that does
 * not match the current generation.
 */
#define EVENT_ID(e)   ((e) & 0xFFu)
#define EVENT_GEN(e)  ((e) >> 8u)
#define EVENT_MAKE(id, gen) ((uint32_t)(id) | ((uint32_t)(gen) << 8u))

typedef enum {
    ReaderStateScanning,
    ReaderStateReading,
    ReaderStateError,
    ReaderStateNotice, // save result / blocked-action message, auto-returns
    ReaderStateEmulating,
} ReaderState;

// Custom events posted from worker threads / the timer to the GUI thread.
typedef enum {
    ReaderEventPhaseTimeout = 100,
    ReaderEventAnimTick,
    ReaderEventNfcScanned,
    ReaderEventNfcRead,
    ReaderEventLfRead,
    ReaderEventError,
    ReaderEventActionSave,
    ReaderEventActionEmulate,
    ReaderEventActionRescan,
    ReaderEventActionExit,
    ReaderEventNoticeDone, // NOTICE_MS elapsed
} ReaderCustomEvent;

// Views registered with the dispatcher.
typedef enum {
    ReaderViewScan = 0,
    ReaderViewInfo = 1,
    ReaderViewActions = 2,
} ReaderView;

typedef struct {
    ReaderState state;
    uint8_t frame; // animation counter, bumped every ANIM_PERIOD_MS
    bool lf; // true while the LF phase is active
    char notice_title[24];
    char notice_l1[32];
    char notice_l2[48];
    char emu_label[32];
} ReaderModel;

// What the info/actions screens currently describe. Set by the read handlers,
// consumed by reader_do_save()/reader_do_emulate() and the payment-card check.
typedef enum {
    ReaderCardNone,
    ReaderCardNfc,
    ReaderCardLf,
} ReaderCardKind;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    TextBox* text_box;
    Submenu* actions;
    FuriString* info_text; // backing store for the TextBox; must outlive the text pointer
    FuriTimer* phase_timer;
    FuriTimer* anim_timer;
    ReaderView current_view; // kept in sync by reader_switch_view()

    // NFC side
    Nfc* nfc;
    NfcScanner* scanner;
    NfcPoller* poller;
    NfcListener* listener; // NFC emulation, NULL when idle
    NfcDevice* device;
    NfcProtocol display_protocol; // most-derived protocol, used for the name/chain
    NfcProtocol poll_protocol; // protocol the poller actually runs (ids 0..11 only)
    EmvData emv; // filled by emv_read() when poll_protocol is ISO14443-4A
    uint8_t mfc_pass; // MfClassic key pass: 0 = key A, 1 = key B
    uint8_t mfc_sector; // MfClassic next sector to offer a key for

    // LF side
    ProtocolDict* dict;
    LFRFIDWorker* worker;
    bool lf_reading; // a read session is currently started
    bool lf_emulating; // an emulate session is currently active
    bool lf_thread_running; // the worker thread exists and must be joined
    ProtocolId lf_protocol;

    // Filled by the LF worker callback path, consumed on the GUI thread.
    uint8_t scratch_id[ID_MAX_LEN];
    size_t scratch_id_len;

    bool lf_phase; // which phase is currently running
    ReaderCardKind card; // what the info/actions screens currently describe
    // Read directly (no with_view_model) by reader_phase_timer_callback(),
    // which runs on the TimersSrv thread: with_view_model() from there was
    // observed to deadlock against furi_timer_start() on the GUI thread
    // (both funnel through the same FreeRTOS timer command queue/task), so
    // this mirrors the existing gen/lf_phase pattern of plain cross-thread
    // fields instead of the model's mutex.
    bool notice_active; // ReaderStateNotice is currently showing
    uint32_t gen; // bumped on every phase change; only touched by the GUI thread
} ReaderApp;

/* ----------------------------- helpers ------------------------------ */

/*
 * NfcProtocolNum and NfcProtocolInvalid are NOT stable across firmware forks:
 * Momentum appends Ntag4xx/Type4Tag/Emv, which moves Invalid from 12 to 16. A
 * FAP compiled against one fork therefore cannot compare a protocol id the
 * running firmware returned against its own copy of those sentinels, and
 * feeding the mismatched value back into nfc_poller_alloc() trips
 * furi_check(protocol < NfcProtocolNum).
 *
 * The protocols below sit at the head of the enum and have the same values on
 * official and Momentum (ids 0..11 verified), so relationships are resolved
 * with nfc_protocol_has_parent() — evaluated by the firmware, against ids we
 * know are valid — and sentinels are never touched.
 */
static const NfcProtocol reader_base_protocols[] = {
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolSt25tb,
};

// True for a transport protocol, false for anything layered on top of one.
static bool protocol_is_base(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(reader_base_protocols); i++) {
        if(p == reader_base_protocols[i]) return true;
    }
    return false;
}

// Pollable protocols, most-derived first. Anything the scanner returns that is
// not in this list (Ntag4xx/Type4Tag/Emv on Momentum, future fork additions)
// resolves to an ancestor from this list and never reaches nfc_poller_alloc.
// Iso14443_4b and Slix are deliberately absent: their poller event enums are
// unverified, so those cards fall back to their transports (3b / ISO15693-3).
static const NfcProtocol reader_pollable_protocols[] = {
    NfcProtocolMfUltralight,
    NfcProtocolMfClassic,
    NfcProtocolIso14443_4a,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolSt25tb,
};

// The most-derived protocol we can safely poll for `p`.
static NfcProtocol reader_poll_protocol(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(reader_pollable_protocols); i++) {
        NfcProtocol q = reader_pollable_protocols[i];
        if(p == q || nfc_protocol_has_parent(p, q)) return q;
    }
    return NfcProtocolIso14443_3a; // unreachable for scanner output
}

// Protocols nfc_listener_alloc() can actually emulate. Two things must both
// hold, checked against Momentum mntm-dev 42630e91 (identical to official
// 1.4.3): the protocol's own entry in nfc_listeners_api[] must be non-NULL
// (nfc_listener_alloc furi_check()s exactly that), AND every ancestor's entry
// must be non-NULL too (nfc_listener_list_alloc() walks the whole parent
// chain and calls each ancestor's ->alloc() with no NULL check at all — an
// unchecked crash, not even a furi_check). Iso14443_3b and St25tb are NULL;
// every protocol below either has no parent or descends only from
// Iso14443_3a (non-NULL), so the whole chain is safe for each entry here.
static const NfcProtocol reader_emulatable_protocols[] = {
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_4a,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolMfUltralight,
    NfcProtocolMfClassic,
};

static bool reader_protocol_emulatable(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(reader_emulatable_protocols); i++) {
        if(p == reader_emulatable_protocols[i]) return true;
    }
    return false;
}

// Per-protocol read bound: in-callback work (EMV APDU chain, Classic key
// passes, ISO15693 full block dump) needs longer than a bare transport
// activation.
static uint32_t reader_read_timeout_for(NfcProtocol p) {
    switch(p) {
    case NfcProtocolMfClassic:
        return 12000; // 2 key passes x up to 80 sector requests
    case NfcProtocolMfUltralight:
        return 8000;
    case NfcProtocolIso15693_3:
        return 8000; // full block dump inside activate
    case NfcProtocolFelica:
        return 6000;
    case NfcProtocolIso14443_4a:
        return EMV_READ_TIMEOUT_MS; // APDU chain in-callback
    default:
        return READ_TIMEOUT_MS; // plain transports
    }
}

// Filesystem-safe stem: protocol names contain '/' and ' '
// ("NTAG/Ultralight", "Mifare Classic", "PAC/Stanley"), which would break
// the path or create directories.
static void reader_sanitize(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for(; src && src[i] && i + 1 < cap; i++) {
        char c = src[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        dst[i] = ok ? c : '_';
    }
    dst[i] = '\0';
}

static void reader_build_path(
    FuriString* out,
    const char* dir,
    const char* type,
    const uint8_t* id,
    size_t id_len,
    const char* ext) {
    char stem[40];
    reader_sanitize(stem, sizeof(stem), type);
    furi_string_printf(out, "%s/%s_", dir, stem);
    for(size_t i = 0; i < id_len; i++) furi_string_cat_printf(out, "%02X", id[i]);
    furi_string_cat_str(out, ext);
}

// nfc_device_save()/lfrfid_dict_file_save() open the file with
// *_open_always(), which does not create the directory.
static bool reader_ensure_dir(const char* dir) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool ok = storage_simply_mkdir(storage, dir); // true when it already exists
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* ------------------------------ drawing ----------------------------- */

static void draw_centered(Canvas* canvas, int cy, const char* str) {
    int w = canvas_string_width(canvas, str);
    canvas_draw_str(canvas, (128 - w) / 2, cy, str);
}

// Like draw_centered(), but truncates with "..." until the string fits inside
// `max` pixels, so a long file name or protocol name never runs off-screen.
static void draw_centered_fit(Canvas* canvas, int cy, const char* str, int max) {
    char buf[56];
    snprintf(buf, sizeof(buf), "%s", str);
    size_t len = strlen(buf);
    while(len > 3 && canvas_string_width(canvas, buf) > max) {
        buf[--len] = '\0';
        buf[len - 1] = '.';
        buf[len - 2] = '.';
    }
    draw_centered(canvas, cy, buf);
}

// Full-width inverted title bar.
static void draw_title_bar(Canvas* canvas, const char* title) {
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    int w = canvas_string_width(canvas, title);
    canvas_draw_str(canvas, (128 - w) / 2, 10, title);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
}

// Small card silhouette with a chip, centred on (cx, cy).
static void draw_card_icon(Canvas* canvas, int cx, int cy) {
    canvas_draw_rframe(canvas, cx - 10, cy - 7, 21, 15, 2);
    canvas_draw_frame(canvas, cx - 7, cy - 4, 7, 6);
    canvas_draw_line(canvas, cx - 7, cy - 1, cx - 1, cy - 1);
    canvas_draw_line(canvas, cx - 4, cy - 4, cx - 4, cy + 1);
    canvas_draw_line(canvas, cx + 1, cy + 4, cx + 8, cy + 4);
}

// Radar: three arcs on each side of the card, expanding outwards and fading
// out (the outermost one is drawn sparsely) as `frame` advances.
static void draw_radar(Canvas* canvas, int cx, int cy, uint8_t frame) {
    for(int i = 0; i < 3; i++) {
        int r = 14 + ((frame + i * 4) % 12);
        // The widest ring is thinned out so the pulse reads as a fade.
        int step = (r > 22) ? 8 : 4;
        for(int a = -38; a <= 38; a += step) {
            float rad = (float)a * (float)M_PI / 180.0f;
            int dx = (int)(cosf(rad) * (float)r);
            int dy = (int)(sinf(rad) * (float)r);
            canvas_draw_dot(canvas, cx + dx, cy + dy);
            canvas_draw_dot(canvas, cx - dx, cy + dy);
        }
    }
}

static void draw_dots(Canvas* canvas, int x, int y, uint8_t frame) {
    int n = (frame / 4) % 4; // 0..3 dots, ~320 ms per step
    for(int i = 0; i < n; i++) {
        canvas_draw_dot(canvas, x + i * 4, y);
        canvas_draw_dot(canvas, x + i * 4 + 1, y);
    }
}

static void draw_cross(Canvas* canvas, int x, int y) {
    canvas_draw_line(canvas, x, y, x + 10, y + 10);
    canvas_draw_line(canvas, x + 1, y, x + 11, y + 10);
    canvas_draw_line(canvas, x + 10, y, x, y + 10);
    canvas_draw_line(canvas, x + 11, y, x + 1, y + 10);
}

static void reader_draw_callback(Canvas* canvas, void* model) {
    ReaderModel* m = model;
    canvas_clear(canvas);
    draw_title_bar(canvas, "UNIVERSAL READER");

    switch(m->state) {
    case ReaderStateScanning: {
        const int cx = 64, cy = 30;
        draw_radar(canvas, cx, cy, m->frame);
        draw_card_icon(canvas, cx, cy);

        const char* active = m->lf ? "< 125 kHz RFID >" : "< 13.56 MHz NFC >";
        const char* idle = m->lf ? "13.56 MHz NFC" : "125 kHz RFID";

        // Active band boxed, the idle one left plain underneath.
        canvas_draw_rframe(canvas, 10, 44, 108, 12, 3);
        draw_centered(canvas, 53, active);

        int idle_w = canvas_string_width(canvas, idle);
        canvas_draw_str(canvas, (128 - idle_w) / 2 - 8, 63, idle);
        draw_dots(canvas, (128 + idle_w) / 2 - 2, 62, m->frame);
        break;
    }

    case ReaderStateReading: {
        draw_card_icon(canvas, 64, 28);
        draw_centered(canvas, 48, "Reading card");

        // Progress bar with a block sweeping left to right.
        canvas_draw_rframe(canvas, 14, 53, 100, 8, 2);
        int pos = (m->frame * 3) % 116; // 0..115, wraps past the right edge
        int x = 16 + pos - 24;
        int w = 24;
        if(x < 16) {
            w -= (16 - x);
            x = 16;
        }
        if(x + w > 112) w = 112 - x;
        if(w > 0) canvas_draw_box(canvas, x, 55, w, 4);
        break;
    }

    case ReaderStateError:
        draw_cross(canvas, 8, 24);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 26, 34, "Read failed");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 26, 45, "retry");
        canvas_draw_str(canvas, 2, 63, "OK: rescan   Back: exit");
        break;

    case ReaderStateNotice:
        canvas_set_font(canvas, FontPrimary);
        draw_centered(canvas, 28, m->notice_title);
        canvas_set_font(canvas, FontSecondary);
        draw_centered_fit(canvas, 42, m->notice_l1, 124);
        draw_centered_fit(canvas, 53, m->notice_l2, 124);
        break;

    case ReaderStateEmulating:
        draw_radar(canvas, 64, 30, m->frame);
        draw_card_icon(canvas, 64, 30);
        canvas_draw_rframe(canvas, 10, 44, 108, 12, 3);
        draw_centered_fit(canvas, 53, m->emu_label, 100);
        draw_centered(canvas, 63, "Back: stop");
        break;
    }
}

/* --------------------------- phase lifecycle ------------------------ */

static void reader_start_nfc_phase(ReaderApp* app);
static void reader_start_lf_phase(ReaderApp* app);

static void reader_stop_nfc(ReaderApp* app) {
    if(app->listener) {
        nfc_listener_stop(app->listener);
        nfc_listener_free(app->listener);
        app->listener = NULL;
    }
    if(app->poller) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
        app->poller = NULL;
    }
    if(app->scanner) {
        nfc_scanner_stop(app->scanner);
        nfc_scanner_free(app->scanner);
        app->scanner = NULL;
    }
}

/*
 * lfrfid_worker_stop() only asks the worker thread to leave read/emulate mode;
 * it does not wait for it. If the NFC side grabs the radio while that teardown
 * is still in flight the firmware wedges hard enough to take USB down with it,
 * so the worker thread is joined here and re-created in
 * reader_start_lf_phase()/reader_start_lf_emulation(). That keeps every
 * transition a strict stop -> release -> start sequence.
 */
static void reader_stop_lf(ReaderApp* app) {
    if(app->lf_reading || app->lf_emulating) {
        lfrfid_worker_stop(app->worker);
        app->lf_reading = false;
        app->lf_emulating = false;
    }
    if(app->lf_thread_running) {
        lfrfid_worker_stop_thread(app->worker);
        app->lf_thread_running = false;
    }
}

// Tear down whatever phase is running and cancel a pending phase timeout.
static void reader_stop_all(ReaderApp* app) {
    furi_timer_stop(app->phase_timer);
    app->notice_active = false;
    reader_stop_nfc(app);
    reader_stop_lf(app);
}

// The only place that changes views. Keeps current_view (which the navigation
// callback reads) and the animation timer in sync: only the scan view animates,
// so a timer left running on a static screen would post a tick into the
// dispatcher every ANIM_PERIOD_MS for nothing.
static void reader_switch_view(ReaderApp* app, ReaderView view) {
    app->current_view = view;
    if(view == ReaderViewScan) {
        furi_timer_start(app->anim_timer, furi_ms_to_ticks(ANIM_PERIOD_MS));
    } else {
        furi_timer_stop(app->anim_timer);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

static void reader_set_scanning(ReaderApp* app, bool lf) {
    with_view_model(
        app->view,
        ReaderModel * m,
        {
            m->state = ReaderStateScanning;
            m->lf = lf;
        },
        true);
}

// GUI thread only. Shows a message on the status view (reusing ReaderViewScan)
// and auto-returns to the report after NOTICE_MS. Used for save results and
// blocked-action explanations; callers stop their own hardware before calling
// this, so it never touches a radio itself.
static void reader_show_notice(ReaderApp* app, const char* title, const char* l1, const char* l2) {
    reader_stop_all(app);
    app->gen++;
    app->notice_active = true;
    with_view_model(
        app->view,
        ReaderModel * m,
        {
            m->state = ReaderStateNotice;
            snprintf(m->notice_title, sizeof(m->notice_title), "%s", title);
            snprintf(m->notice_l1, sizeof(m->notice_l1), "%s", l1 ? l1 : "");
            snprintf(m->notice_l2, sizeof(m->notice_l2), "%s", l2 ? l2 : "");
        },
        true);
    reader_switch_view(app, ReaderViewScan);
    furi_timer_stop(app->anim_timer); // the notice screen is static, unlike Emulating
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(NOTICE_MS));
}

/* --------------------------- nfc callbacks -------------------------- */

// Common tail of every poller path: snapshot the card into the device (a deep
// copy, so the data outlives the poller) and hand over to the GUI thread.
// Runs on the NFC worker thread.
static NfcCommand reader_nfc_done(ReaderApp* app, NfcProtocol polled) {
    nfc_device_set_data(app->device, polled, nfc_poller_get_data(app->poller));
    FURI_LOG_I(TAG, "NFC read: %s", nfc_device_get_protocol_name(app->display_protocol));
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(ReaderEventNfcRead, app->gen));
    return NfcCommandStop;
}

// Runs on the NFC worker thread. The callback must answer the poller's
// requests (mode, keys, auth context) and stop on a terminal event; the exact
// set differs per protocol. Only compile-time protocol ids 0..11 ever reach
// this switch (see reader_poll_protocol).
static NfcCommand reader_poller_callback(NfcGenericEvent event, void* context) {
    ReaderApp* app = context;

    switch(event.protocol) {
    case NfcProtocolIso14443_3a:
        if(((Iso14443_3aPollerEvent*)event.event_data)->type == Iso14443_3aPollerEventTypeReady) {
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolIso14443_3b:
        if(((Iso14443_3bPollerEvent*)event.event_data)->type == Iso14443_3bPollerEventTypeReady) {
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolIso15693_3:
        // Ready means inventory + system info + all blocks were read already.
        if(((Iso15693_3PollerEvent*)event.event_data)->type == Iso15693_3PollerEventTypeReady) {
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolSt25tb:
        if(((St25tbPollerEvent*)event.event_data)->type == St25tbPollerEventTypeReady) {
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolFelica: {
        FelicaPollerEvent* e = event.event_data;
        switch(e->type) {
        case FelicaPollerEventTypeReady:
        case FelicaPollerEventTypeIncomplete: // partial dump still worth showing
            return reader_nfc_done(app, event.protocol);
        case FelicaPollerEventTypeRequestAuthContext:
            // alloc does not initialise skip_auth (malloc garbage) and the
            // activate handler branches on it.
            e->data->auth_context->skip_auth = true;
            break;
        default:
            break; // Error: keep polling until the timeout
        }
        break;
    }
    case NfcProtocolIso14443_4a:
        if(((Iso14443_4aPollerEvent*)event.event_data)->type == Iso14443_4aPollerEventTypeReady) {
            // The read-only EMV chain must run here:
            // iso14443_4a_poller_send_block() is only legal inside the callback.
            memset(&app->emv, 0, sizeof(app->emv));
            emv_read((Iso14443_4aPoller*)event.instance, &app->emv);
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolMfUltralight: {
        MfUltralightPollerEvent* e = event.event_data;
        switch(e->type) {
        case MfUltralightPollerEventTypeAuthRequest:
            // The firmware reads skip_auth uninitialised otherwise (verified).
            e->data->auth_context.skip_auth = true;
            break;
        case MfUltralightPollerEventTypeReadSuccess:
        case MfUltralightPollerEventTypeReadFailed: // partial data still worth showing
            return reader_nfc_done(app, event.protocol);
        default:
            break; // RequestMode: the firmware pre-sets Read
        }
        break;
    }
    case NfcProtocolMfClassic: {
        MfClassicPollerEvent* e = event.event_data;
        switch(e->type) {
        case MfClassicPollerEventTypeRequestMode:
            // The firmware furi_crash()es on an uninitialised mode. Read mode
            // only: the DictAttack modes are ABI-unsafe on Momentum.
            e->data->poller_mode.mode = MfClassicPollerModeRead;
            e->data->poller_mode.data = NULL;
            break;
        case MfClassicPollerEventTypeRequestReadSector: {
            // Offer the factory-default transport key (FF FF FF FF FF FF, the
            // NXP shipping default, for reading your own blank/personal cards)
            // for every sector: key A pass first, then a key B pass over the
            // sectors still unread. key_provided = false ends the read; the
            // firmware then emits Success. Never touch key_request_data (only
            // used by dict-attack modes, ABI-unsafe on Momentum).
            MfClassicPollerEventDataReadSectorRequest* r = &e->data->read_sector_request_data;
            static const uint8_t transport_key[MF_CLASSIC_KEY_SIZE] =
                {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            const MfClassicData* d = (const MfClassicData*)nfc_poller_get_data(app->poller);
            uint8_t sectors = mf_classic_get_total_sectors_num(d->type);
            for(;;) {
                if(app->mfc_sector >= sectors) {
                    if(app->mfc_pass == 0) {
                        app->mfc_pass = 1;
                        app->mfc_sector = 0;
                        continue;
                    }
                    r->key_provided = false;
                    break;
                }
                if(app->mfc_pass == 1 && mf_classic_is_sector_read(d, app->mfc_sector)) {
                    app->mfc_sector++;
                    continue;
                }
                r->sector_num = app->mfc_sector++;
                memcpy(r->key.data, transport_key, MF_CLASSIC_KEY_SIZE);
                r->key_type = app->mfc_pass == 0 ? MfClassicKeyTypeA : MfClassicKeyTypeB;
                r->key_provided = true;
                break;
            }
            break;
        }
        case MfClassicPollerEventTypeSuccess:
        case MfClassicPollerEventTypeFail:
            return reader_nfc_done(app, event.protocol);
        default:
            break; // CardDetected/CardLost/DataUpdate/...: keep polling
        }
        break;
    }
    default:
        break;
    }

    return NfcCommandContinue; // keep polling until a terminal event or the timeout
}

// Runs on the NFC worker thread.
static void reader_scanner_callback(NfcScannerEvent event, void* context) {
    ReaderApp* app = context;
    if(event.type != NfcScannerEventTypeDetected || event.data.protocol_num == 0) {
        return;
    }

    // The scanner already prefers children over their parents; picking the
    // first non-transport entry gives the friendly name (e.g. "Mifare Classic")
    // rather than the transport underneath it.
    NfcProtocol best = event.data.protocols[0];
    for(size_t i = 0; i < event.data.protocol_num; i++) {
        if(!protocol_is_base(event.data.protocols[i])) {
            best = event.data.protocols[i];
            break;
        }
    }

    app->display_protocol = best;
    app->poll_protocol = reader_poll_protocol(best);
    FURI_LOG_I(TAG, "NFC detected: %s", nfc_device_get_protocol_name(best));
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(ReaderEventNfcScanned, app->gen));
}

// NFC emulation listener callback. Runs on the NFC worker thread; the
// listener itself answers reader commands from firmware-side protocol state,
// so there is nothing for the app to do here but keep going until Back stops
// the listener from the GUI thread.
static NfcCommand reader_listener_callback(NfcGenericEvent event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return NfcCommandContinue;
}

/* --------------------------- lf callbacks --------------------------- */

// Runs on the LF worker thread. Only ReadDone means a decoded card; the
// other results are intermediate progress reports.
static void reader_lf_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    ReaderApp* app = context;
    if(result != LFRFIDWorkerReadDone) return;

    app->lf_protocol = protocol;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(ReaderEventLfRead, app->gen));
}

/* ---------------------------- phase starts -------------------------- */

// Runs on the TimersSrv thread, not the GUI thread. Branches on
// notice_active (a plain field, not the view model - see its declaration)
// so the one-shot phase_timer can serve both the scan/read phase timeout
// and the notice auto-dismiss without needing a second timer.
static void reader_phase_timer_callback(void* context) {
    ReaderApp* app = context;
    uint8_t id = app->notice_active ? ReaderEventNoticeDone : ReaderEventPhaseTimeout;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(id, app->gen));
}

static void reader_anim_timer_callback(void* context) {
    ReaderApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, ReaderEventAnimTick);
}

// GUI thread only. Bumping the generation invalidates every event still queued
// from the phase we are leaving.
static void reader_start_nfc_phase(ReaderApp* app) {
    reader_stop_all(app);
    app->gen++;
    app->lf_phase = false;
    reader_set_scanning(app, false);
    // Rescan from the info screen (Back) must land on the scan view again.
    reader_switch_view(app, ReaderViewScan);

    FURI_LOG_D(TAG, "phase: NFC (gen %lu)", (unsigned long)app->gen);
    app->scanner = nfc_scanner_alloc(app->nfc);
    nfc_scanner_start(app->scanner, reader_scanner_callback, app);
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(NFC_PHASE_MS));
}

static void reader_start_lf_phase(ReaderApp* app) {
    reader_stop_all(app);
    app->gen++;
    app->lf_phase = true;
    reader_set_scanning(app, true);
    reader_switch_view(app, ReaderViewScan);

    FURI_LOG_D(TAG, "phase: LF (gen %lu)", (unsigned long)app->gen);
    lfrfid_worker_start_thread(app->worker);
    app->lf_thread_running = true;
    lfrfid_worker_read_start(app->worker, LFRFIDWorkerReadTypeAuto, reader_lf_callback, app);
    app->lf_reading = true;
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(LF_PHASE_MS));
}

// GUI thread only. Puts the status view (ReaderViewScan) into the emulating
// sub-state; reader_switch_view() starts anim_timer so the radar animates.
static void reader_enter_emulating(ReaderApp* app, const char* type) {
    with_view_model(
        app->view,
        ReaderModel * m,
        {
            m->state = ReaderStateEmulating;
            snprintf(m->emu_label, sizeof(m->emu_label), "%s", type ? type : "card");
        },
        true);
    reader_switch_view(app, ReaderViewScan);
}

// GUI thread only. Caller (reader_do_emulate()) has already checked
// reader_protocol_emulatable(app->poll_protocol) and that this is not a
// payment card.
static void reader_start_nfc_emulation(ReaderApp* app) {
    reader_stop_all(app); // scanner/poller/LF worker released and joined first
    app->gen++;
    const NfcDeviceData* data = nfc_device_get_data(app->device, app->poll_protocol);
    app->listener = nfc_listener_alloc(app->nfc, app->poll_protocol, data);
    nfc_listener_start(app->listener, reader_listener_callback, app);
    FURI_LOG_I(TAG, "emulating NFC: %s", nfc_device_get_protocol_name(app->display_protocol));
    reader_enter_emulating(app, nfc_device_get_protocol_name(app->display_protocol));
}

// GUI thread only. The LF phase owns the worker thread; it is already stopped
// on the info/actions screens, so emulation starts it again (see reader_stop_lf).
static void reader_start_lf_emulation(ReaderApp* app) {
    reader_stop_all(app);
    app->gen++;
    lfrfid_worker_start_thread(app->worker);
    app->lf_thread_running = true;
    lfrfid_worker_emulate_start(app->worker, (LFRFIDProtocol)app->lf_protocol);
    app->lf_emulating = true;
    const char* name = protocol_dict_get_name(app->dict, app->lf_protocol);
    FURI_LOG_I(TAG, "emulating LF: %s", name ? name : "Unknown");
    reader_enter_emulating(app, name ? name : "LF card");
}

/* --------------------------- view callbacks ------------------------- */

// Back that no view consumed. Runs on the GUI thread (input path), so starting
// a phase or switching views here is legal.
//
// This has to be the dispatcher's navigation callback rather than a module
// view's previous_callback: view_previous() passes view->context, and
// text_box_alloc()/submenu_alloc() set that to the TextBox/Submenu itself, so
// a previous_callback would receive that pointer to use as a ReaderApp*.
// Measured on the device: prev_ctx == text_box (0x2000A578), app was
// 0x2000A5C0 — dereferencing it crashed the firmware. The dispatcher passes
// event_context, i.e. the app, so every view transition funnels through here
// and reader_switch_view() instead of a per-view previous_callback.
static bool reader_navigation_callback(void* context) {
    ReaderApp* app = context;

    // The info TextBox never consumes Back; the scan view consumes only short
    // and repeat presses, so a long Back from there lands here too. Back on
    // the report opens the actions menu instead of rescanning.
    if(app->current_view == ReaderViewInfo) {
        submenu_set_selected_item(app->actions, 0);
        reader_switch_view(app, ReaderViewActions);
        return true;
    }

    // The actions Submenu does not consume Back either (verified: its input
    // callback never checks InputKeyBack); it returns to the report.
    if(app->current_view == ReaderViewActions) {
        reader_switch_view(app, ReaderViewInfo);
        return true;
    }

    // Leaving: release the radio now and retire every event still queued from
    // the phase we are killing, then let the dispatcher stop. run() returns and
    // reader_app_free() does the rest.
    reader_stop_all(app);
    app->gen++;
    return false;
}

// Actions submenu item callback. Runs on the GUI thread, but routes through
// the dispatcher anyway so that every radio start/stop stays inside
// reader_custom_event_callback() (project invariant). `index` is the
// ReaderCustomEvent value the item was registered with.
static void reader_action_callback(void* context, uint32_t index) {
    ReaderApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(index, app->gen));
}
// EMV / bank card. app->emv is populated by emv_read() (called from inside
// reader_poller_callback whenever poll_protocol is ISO14443-4A), so ppse_ok /
// aid_count is the primary, reliable signal. The name compare is a fallback
// for a card the scanner itself classified as Momentum's NfcProtocolEmv (id
// 14, not in the SDK enum this app compiles against) whose own PPSE select
// nonetheless failed; the string comes from the firmware's own device table
// (EMV_PROTOCOL_NAME "EMV", verified in Momentum lib/nfc/protocols/emv/emv.c).
static bool reader_is_payment_card(const ReaderApp* app) {
    if(app->card != ReaderCardNfc) return false;
    if(app->emv.ppse_ok || app->emv.aid_count > 0) return true;
    const char* name = nfc_device_get_protocol_name(app->display_protocol);
    return name && strcmp(name, "EMV") == 0;
}


static void reader_do_save(ReaderApp* app) {
    if(app->card == ReaderCardNone) return;

    FuriString* path = furi_string_alloc();
    bool ok = false;
    const char* dir;

    if(app->card == ReaderCardLf) {
        dir = EXT_PATH("lfrfid");
        const char* name = protocol_dict_get_name(app->dict, app->lf_protocol);
        reader_build_path(
            path, dir, name ? name : "Unknown", app->scratch_id, app->scratch_id_len, ".rfid");
        ok = reader_ensure_dir(dir) &&
             lfrfid_dict_file_save(app->dict, app->lf_protocol, furi_string_get_cstr(path));
    } else if(reader_is_payment_card(app)) {
        // EMV / bank card: save ALL data (PAN, expiry, name, AIDs, track2, log)
        // to a dedicated .emv file, not just the base ISO14443-4A UID.
        dir = EXT_PATH("nfc");
        size_t uid_len = 0;
        const uint8_t* uid = nfc_device_get_uid(app->device, &uid_len);
        reader_build_path(
            path, dir, "EMV", uid, uid_len, ".emv");
        ok = reader_ensure_dir(dir) && emv_save(&app->emv, furi_string_get_cstr(path));
    } else {
        dir = EXT_PATH("nfc");
        size_t uid_len = 0;
        const uint8_t* uid = nfc_device_get_uid(app->device, &uid_len);
        reader_build_path(
            path, dir, nfc_device_get_protocol_name(app->display_protocol), uid, uid_len, ".nfc");
        ok = reader_ensure_dir(dir) && nfc_device_save(app->device, furi_string_get_cstr(path));
    }

    const char* full = furi_string_get_cstr(path);
    const char* base = strrchr(full, '/');
    base = base ? base + 1 : full;
    FURI_LOG_I(TAG, "save %s: %s", ok ? "ok" : "FAILED", full);
    reader_show_notice(app, ok ? "Saved" : "Save failed", dir, base);
    furi_string_free(path);
}


static void reader_do_emulate(ReaderApp* app) {
    if(app->card == ReaderCardNone) return;

    if(app->card == ReaderCardLf) {
        reader_start_lf_emulation(app);
        return;
    }
    // EMV / bank card: emulate at ISO14443-4A level (UID + ATS).
    // The card presents its full data (PAN, expiry, AIDs, track2, log) to any
    // reader that queries it, just like the original card.
    if(reader_is_payment_card(app)) {
        if(!reader_protocol_emulatable(app->poll_protocol)) {
            reader_show_notice(
                app, "Blocked", "No emulation for", nfc_device_get_protocol_name(app->poll_protocol));
            return;
        }
        reader_start_nfc_emulation(app);
        return;
    }
    if(!reader_protocol_emulatable(app->poll_protocol)) {
        reader_show_notice(
            app, "Blocked", "No emulation for", nfc_device_get_protocol_name(app->poll_protocol));
        return;
    }
    reader_start_nfc_emulation(app);
}

static bool reader_custom_event_callback(void* context, uint32_t event) {
    ReaderApp* app = context;

    if(event == ReaderEventAnimTick) {
        with_view_model(app->view, ReaderModel * m, { m->frame++; }, true);
        return true;
    }

    // Anything produced by a previous phase is stale — see EVENT_MAKE above.
    if(EVENT_GEN(event) != app->gen) {
        FURI_LOG_D(
            TAG,
            "drop stale event %lu (gen %lu != %lu)",
            (unsigned long)EVENT_ID(event),
            (unsigned long)EVENT_GEN(event),
            (unsigned long)app->gen);
        return true;
    }

    switch(EVENT_ID(event)) {
    case ReaderEventPhaseTimeout:
        // Either the band was empty, or a card was pulled away mid-read.
        if(app->poller) FURI_LOG_I(TAG, "read timed out, resuming scan");
        if(app->lf_phase) {
            reader_start_nfc_phase(app);
        } else {
            reader_start_lf_phase(app);
        }
        return true;

    case ReaderEventNfcScanned:
        // The scanner re-detects in a loop, so this can arrive more than once.
        // Only the first one may start a poller.
        if(!app->scanner || app->poller) return true;

        furi_timer_stop(app->phase_timer);
        nfc_scanner_stop(app->scanner);
        nfc_scanner_free(app->scanner);
        app->scanner = NULL;

        // Retires any duplicate ReaderEventNfcScanned still in the queue.
        app->gen++;

        with_view_model(app->view, ReaderModel * m, { m->state = ReaderStateReading; }, true);
        // Restart every per-read state so a rescan begins cleanly.
        app->mfc_pass = 0;
        app->mfc_sector = 0;
        memset(&app->emv, 0, sizeof(app->emv)); // no stale bank data from a previous card
        app->poller = nfc_poller_alloc(app->nfc, app->poll_protocol);
        nfc_poller_start(app->poller, reader_poller_callback, app);
        // Bounded read: a card removed now must not leave us stuck on "Reading".
        // Longer bounds cover in-callback work (EMV APDU chain, Classic key
        // passes): nfc_poller_stop() joins the worker thread, so a short
        // timeout would block the GUI thread mid-read.
        furi_timer_start(
            app->phase_timer, furi_ms_to_ticks(reader_read_timeout_for(app->poll_protocol)));
        return true;

    case ReaderEventNfcRead:
        if(!app->poller) return true;
        reader_stop_all(app);
        app->gen++;
        app->card = ReaderCardNfc;
        text_box_reset(app->text_box); // drop the stale text pointer before rebuilding
        furi_string_reset(app->info_text);
        card_info_format_nfc(app->info_text, app->device, app->display_protocol, &app->emv);
        if(reader_is_payment_card(app)) {
            furi_string_cat_str(
                app->info_text,
                "\n[Policy] Bank card: emulation disabled;\nsave stores UID/ATS only.\n");
        }
        furi_string_cat_str(app->info_text, "\n[Back] = actions menu\n");
        submenu_set_header(app->actions, nfc_device_get_protocol_name(app->display_protocol));
        text_box_set_font(app->text_box, TextBoxFontText);
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        text_box_set_text(app->text_box, furi_string_get_cstr(app->info_text));
        reader_switch_view(app, ReaderViewInfo);
        return true;

    case ReaderEventLfRead: {
        if(!app->lf_phase || !app->lf_reading) return true;
        reader_stop_all(app);
        app->gen++;
        app->card = ReaderCardLf;

        const char* name = protocol_dict_get_name(app->dict, app->lf_protocol);
        size_t size = protocol_dict_get_data_size(app->dict, app->lf_protocol);
        if(size > ID_MAX_LEN) size = ID_MAX_LEN;
        protocol_dict_get_data(app->dict, app->lf_protocol, app->scratch_id, size);
        app->scratch_id_len = size;

        FuriString* lf_hex = furi_string_alloc();
        for(size_t i = 0; i < size; i++) {
            furi_string_cat_printf(lf_hex, "%02X", app->scratch_id[i]);
        }
        FURI_LOG_I(
            TAG, "LF read: %s, ID %s", name ? name : "Unknown", furi_string_get_cstr(lf_hex));
        furi_string_free(lf_hex);

        text_box_reset(app->text_box);
        furi_string_reset(app->info_text);
        card_info_format_lf(
            app->info_text, name ? name : "Unknown", app->scratch_id, app->scratch_id_len);
        furi_string_cat_str(app->info_text, "\n[Back] = actions menu\n");
        submenu_set_header(app->actions, name ? name : "Unknown");
        text_box_set_font(app->text_box, TextBoxFontText);
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        text_box_set_text(app->text_box, furi_string_get_cstr(app->info_text));
        reader_switch_view(app, ReaderViewInfo);
        return true;
    }

    case ReaderEventError:
        reader_stop_all(app);
        app->gen++;
        furi_timer_stop(app->anim_timer); // the error screen is static
        with_view_model(app->view, ReaderModel * m, { m->state = ReaderStateError; }, true);
        return true;

    case ReaderEventActionSave:
        reader_do_save(app);
        return true;

    case ReaderEventActionEmulate:
        reader_do_emulate(app);
        return true;

    case ReaderEventActionRescan:
        reader_start_nfc_phase(app);
        return true;

    case ReaderEventActionExit:
        reader_stop_all(app);
        app->gen++;
        view_dispatcher_stop(app->view_dispatcher);
        return true;

    case ReaderEventNoticeDone:
        reader_switch_view(app, ReaderViewInfo);
        return true;

    default:
        return false;
    }
}

static bool reader_input_callback(InputEvent* event, void* context) {
    ReaderApp* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    ReaderState state;
    with_view_model(app->view, ReaderModel * m, { state = m->state; }, false);

    if(state == ReaderStateEmulating) {
        if(event->key == InputKeyBack) {
            reader_stop_all(app); // listener stop+free / LF worker stop+join
            app->gen++;
            reader_switch_view(app, ReaderViewInfo);
            return true;
        }
        return true; // swallow everything else while emulating
    }

    if(state == ReaderStateNotice) {
        if(event->key == InputKeyBack || event->key == InputKeyOk) {
            furi_timer_stop(app->phase_timer); // cancel the pending auto-dismiss
            reader_switch_view(app, ReaderViewInfo);
            return true;
        }
        return true; // swallow everything else while the notice is up
    }

    if(event->key == InputKeyOk && state == ReaderStateError) {
        reader_start_nfc_phase(app);
        return true;
    }

    // Back is deliberately left unconsumed: it falls through to the
    // dispatcher's navigation callback, which owns rescan/menu and exit.

    return false;
}

/* ------------------------------ app life ---------------------------- */

static ReaderApp* reader_app_alloc(void) {
    ReaderApp* app = malloc(sizeof(ReaderApp));
    memset(app, 0, sizeof(ReaderApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->view = view_alloc();

    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(ReaderModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, reader_draw_callback);
    view_set_input_callback(app->view, reader_input_callback);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, reader_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, reader_navigation_callback);
    view_dispatcher_add_view(app->view_dispatcher, ReaderViewScan, app->view);

    app->text_box = text_box_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ReaderViewInfo, text_box_get_view(app->text_box));
    // text_box_set_text() stores the raw pointer, so this string must stay
    // alive and unmodified while the info view is shown.
    app->info_text = furi_string_alloc();
    furi_string_reserve(app->info_text, 8192);

    app->actions = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ReaderViewActions, submenu_get_view(app->actions));
    submenu_add_item(app->actions, "Save", ReaderEventActionSave, reader_action_callback, app);
    submenu_add_item(
        app->actions, "Emulate", ReaderEventActionEmulate, reader_action_callback, app);
    submenu_add_item(
        app->actions, "Rescan", ReaderEventActionRescan, reader_action_callback, app);
    submenu_add_item(app->actions, "Exit", ReaderEventActionExit, reader_action_callback, app);

    app->phase_timer =
        furi_timer_alloc(reader_phase_timer_callback, FuriTimerTypeOnce, app);
    app->anim_timer =
        furi_timer_alloc(reader_anim_timer_callback, FuriTimerTypePeriodic, app);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    reader_switch_view(app, ReaderViewScan); // also starts the animation timer

    app->nfc = nfc_alloc();
    app->device = nfc_device_alloc();

    app->dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    app->worker = lfrfid_worker_alloc(app->dict);
    // The worker thread is owned by the LF phase, not by the app lifetime.

    return app;
}

static void reader_app_free(ReaderApp* app) {
    // Silence the timers first so nothing can post into a dispatcher we are
    // about to tear down, then release the radios.
    furi_timer_stop(app->anim_timer);
    reader_stop_all(app);

    // reader_stop_all() above already joined the worker thread if it was running.
    lfrfid_worker_free(app->worker);
    protocol_dict_free(app->dict);

    nfc_device_free(app->device);
    nfc_free(app->nfc);

    furi_timer_free(app->anim_timer);
    furi_timer_stop(app->phase_timer);
    furi_timer_free(app->phase_timer);

    view_dispatcher_remove_view(app->view_dispatcher, ReaderViewScan);
    view_free(app->view);
    text_box_reset(app->text_box); // release the pointer into info_text first
    view_dispatcher_remove_view(app->view_dispatcher, ReaderViewInfo);
    text_box_free(app->text_box);
    furi_string_free(app->info_text);
    view_dispatcher_remove_view(app->view_dispatcher, ReaderViewActions);
    submenu_free(app->actions);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t universal_card_reader_app(void* p) {
    UNUSED(p);
    ReaderApp* app = reader_app_alloc();

    reader_start_nfc_phase(app);
    view_dispatcher_run(app->view_dispatcher);

    reader_app_free(app);
    return 0;
}
