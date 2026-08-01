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
#include <input/input.h>

#include <nfc/nfc.h>
#include <nfc/nfc_scanner.h>
#include <nfc/nfc_poller.h>
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
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>
#include "emv.h"
#include "card_info.h"

#define TAG "UniCardReader"

#define ID_MAX_LEN     16
#define NFC_PHASE_MS   1200
#define LF_PHASE_MS    1600
#define ANIM_PERIOD_MS 80

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
} ReaderState;

// Custom events posted from worker threads / the timer to the GUI thread.
typedef enum {
    ReaderEventPhaseTimeout = 100,
    ReaderEventAnimTick,
    ReaderEventNfcScanned,
    ReaderEventNfcRead,
    ReaderEventLfRead,
    ReaderEventError,
} ReaderCustomEvent;

// Views registered with the dispatcher.
typedef enum {
    ReaderViewScan = 0,
    ReaderViewInfo = 1,
} ReaderView;

typedef struct {
    ReaderState state;
    uint8_t frame; // animation counter, bumped every ANIM_PERIOD_MS
    bool lf; // true while the LF phase is active
} ReaderModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    TextBox* text_box;
    FuriString* info_text; // backing store for the TextBox; must outlive the text pointer
    FuriTimer* phase_timer;
    FuriTimer* anim_timer;

    // NFC side
    Nfc* nfc;
    NfcScanner* scanner;
    NfcPoller* poller;
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
    bool lf_thread_running; // the worker thread exists and must be joined
    ProtocolId lf_protocol;

    // Filled by the LF worker callback path, consumed on the GUI thread.
    uint8_t scratch_id[ID_MAX_LEN];
    size_t scratch_id_len;

    bool lf_phase; // which phase is currently running
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

/* ------------------------------ drawing ----------------------------- */

static void draw_centered(Canvas* canvas, int cy, const char* str) {
    int w = canvas_string_width(canvas, str);
    canvas_draw_str(canvas, (128 - w) / 2, cy, str);
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
    }
}

/* --------------------------- phase lifecycle ------------------------ */

static void reader_start_nfc_phase(ReaderApp* app);
static void reader_start_lf_phase(ReaderApp* app);

static void reader_stop_nfc(ReaderApp* app) {
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
 * lfrfid_worker_stop() only asks the worker thread to leave read mode; it does
 * not wait for it. If the NFC side grabs the radio while that teardown is still
 * in flight the firmware wedges hard enough to take USB down with it, so the
 * worker thread is joined here and re-created in reader_start_lf_phase(). That
 * keeps the phase switch a strict stop -> release -> start sequence.
 */
static void reader_stop_lf(ReaderApp* app) {
    if(app->lf_reading) {
        lfrfid_worker_stop(app->worker);
        app->lf_reading = false;
    }
    if(app->lf_thread_running) {
        lfrfid_worker_stop_thread(app->worker);
        app->lf_thread_running = false;
    }
}

// Tear down whatever phase is running and cancel a pending phase timeout.
static void reader_stop_all(ReaderApp* app) {
    furi_timer_stop(app->phase_timer);
    reader_stop_nfc(app);
    reader_stop_lf(app);
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

static void reader_phase_timer_callback(void* context) {
    ReaderApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(ReaderEventPhaseTimeout, app->gen));
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
    view_dispatcher_switch_to_view(app->view_dispatcher, ReaderViewScan);

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
    view_dispatcher_switch_to_view(app->view_dispatcher, ReaderViewScan);

    FURI_LOG_D(TAG, "phase: LF (gen %lu)", (unsigned long)app->gen);
    lfrfid_worker_start_thread(app->worker);
    app->lf_thread_running = true;
    lfrfid_worker_read_start(app->worker, LFRFIDWorkerReadTypeAuto, reader_lf_callback, app);
    app->lf_reading = true;
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(LF_PHASE_MS));
}

/* --------------------------- view callbacks ------------------------- */

// Back from the info screen: rescan immediately. Runs on the GUI thread, so
// starting the NFC phase here is legal. VIEW_IGNORE because the phase start
// already switched the view.
static uint32_t reader_info_previous_callback(void* context) {
    reader_start_nfc_phase(context);
    return VIEW_IGNORE;
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
        text_box_reset(app->text_box); // drop the stale text pointer before rebuilding
        furi_string_reset(app->info_text);
        card_info_format_nfc(app->info_text, app->device, app->display_protocol, &app->emv);
        text_box_set_font(app->text_box, TextBoxFontText);
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        text_box_set_text(app->text_box, furi_string_get_cstr(app->info_text));
        view_dispatcher_switch_to_view(app->view_dispatcher, ReaderViewInfo);
        return true;

    case ReaderEventLfRead: {
        if(!app->lf_phase || !app->lf_reading) return true;
        reader_stop_all(app);
        app->gen++;

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
        text_box_set_font(app->text_box, TextBoxFontText);
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        text_box_set_text(app->text_box, furi_string_get_cstr(app->info_text));
        view_dispatcher_switch_to_view(app->view_dispatcher, ReaderViewInfo);
        return true;
    }

    case ReaderEventError:
        reader_stop_all(app);
        app->gen++;
        with_view_model(app->view, ReaderModel * m, { m->state = ReaderStateError; }, true);
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

    if(event->key == InputKeyOk && state == ReaderStateError) {
        reader_start_nfc_phase(app);
        return true;
    }

    if(event->key == InputKeyBack) {
        reader_stop_all(app);
        app->gen++;
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }

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
    view_dispatcher_add_view(app->view_dispatcher, ReaderViewScan, app->view);

    app->text_box = text_box_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ReaderViewInfo, text_box_get_view(app->text_box));
    view_set_previous_callback(
        text_box_get_view(app->text_box), reader_info_previous_callback);
    // text_box_set_text() stores the raw pointer, so this string must stay
    // alive and unmodified while the info view is shown.
    app->info_text = furi_string_alloc();
    furi_string_reserve(app->info_text, 8192);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, ReaderViewScan);

    app->phase_timer =
        furi_timer_alloc(reader_phase_timer_callback, FuriTimerTypeOnce, app);
    app->anim_timer =
        furi_timer_alloc(reader_anim_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->anim_timer, furi_ms_to_ticks(ANIM_PERIOD_MS));

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
