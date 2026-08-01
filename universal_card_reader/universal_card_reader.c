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
#include <stdarg.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
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

#include <lfrfid/lfrfid_worker.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>
#include "emv.h"

#define TAG "UniCardReader"

#define ID_MAX_LEN     16
#define NFC_PHASE_MS   1200
#define LF_PHASE_MS    1600
#define ANIM_PERIOD_MS 80

#define READ_TIMEOUT_MS 2500
#define EMV_READ_TIMEOUT_MS 6000

#define RESULT_MAX_LINES     20
#define RESULT_LINE_LEN      26
#define RESULT_VISIBLE_LINES 5

#define BAND_NFC "NFC 13.56MHz"
#define BAND_LF  "LF 125kHz"

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
    ReaderStateDone,
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

typedef struct {
    ReaderState state;
    uint8_t frame; // animation counter, bumped every ANIM_PERIOD_MS
    bool lf; // true while the LF phase is active / an LF card was read
    char band[16];
    char type_name[48];
    char lines[RESULT_MAX_LINES][RESULT_LINE_LEN];
    uint8_t line_count;
    uint8_t scroll;
} ReaderModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    FuriTimer* phase_timer;
    FuriTimer* anim_timer;

    // NFC side
    Nfc* nfc;
    NfcScanner* scanner;
    NfcPoller* poller;
    NfcDevice* device;
    NfcProtocol display_protocol; // most-derived protocol, used for the name
    NfcProtocol base_protocol; // transport protocol used for polling
    bool emv_capable; // true when base_protocol speaks ISO14443-4A (APDUs)
    EmvData emv; // filled by emv_read() when emv_capable

    // LF side
    ProtocolDict* dict;
    LFRFIDWorker* worker;
    bool lf_reading; // a read session is currently started
    bool lf_thread_running; // the worker thread exists and must be joined
    ProtocolId lf_protocol;

    // Filled by worker callbacks, consumed on the GUI thread.
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
 * The transport-layer protocols below sit at the head of the enum and have the
 * same values everywhere, so we resolve the transport with
 * nfc_protocol_has_parent() — evaluated by the firmware, against ids we know
 * are valid — and never touch a sentinel.
 */
static const NfcProtocol reader_base_protocols[] = {
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolSt25tb,
};

// The transport protocol to poll for `p`, or `p` itself if it is one already.
static NfcProtocol protocol_base(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(reader_base_protocols); i++) {
        if(p == reader_base_protocols[i] || nfc_protocol_has_parent(p, reader_base_protocols[i])) {
            return reader_base_protocols[i];
        }
    }
    return p;
}

// True for a transport protocol, false for anything layered on top of one.
static bool protocol_is_base(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(reader_base_protocols); i++) {
        if(p == reader_base_protocols[i]) return true;
    }
    return false;
}

// True when the card speaks ISO14443-4A, i.e. it can take APDUs. Evaluated by
// the firmware; NfcProtocolIso14443_4a is id 2 in the stock SDK and in Momentum.
static bool protocol_is_iso14443_4a(NfcProtocol p) {
    return p == NfcProtocolIso14443_4a || nfc_protocol_has_parent(p, NfcProtocolIso14443_4a);
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

/* -------------------------- result screen lines ---------------------- */

static void result_reset(ReaderModel* m) {
    m->line_count = 0;
    m->scroll = 0;
}

// Formats one more line into the result screen. A no-op once the line list
// is full, so a chatty card cannot overflow ReaderModel::lines.
static void result_addf(ReaderModel* m, const char* fmt, ...) {
    if(m->line_count >= RESULT_MAX_LINES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(m->lines[m->line_count], RESULT_LINE_LEN, fmt, args);
    va_end(args);
    m->line_count++;
}

// Prints "<label> <hex>" with no separators between bytes, wrapping after 10
// bytes per line; continuation lines are prefixed with two spaces.
static void result_add_hex(ReaderModel* m, const char* label, const uint8_t* d, size_t len) {
    const size_t per_line = 10;
    size_t i = 0;
    do {
        if(m->line_count >= RESULT_MAX_LINES) return;
        char* line = m->lines[m->line_count++];
        int n = snprintf(line, RESULT_LINE_LEN, "%s ", i == 0 ? label : " ");
        if(n < 0) n = 0;
        if((size_t)n >= RESULT_LINE_LEN) n = (int)RESULT_LINE_LEN - 1;

        size_t end = i + per_line;
        if(end > len) end = len;
        for(; i < end && (size_t)n + 2 < RESULT_LINE_LEN; i++) {
            n += snprintf(line + (size_t)n, RESULT_LINE_LEN - (size_t)n, "%02X", d[i]);
        }
    } while(i < len);
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

    case ReaderStateDone: {
        canvas_set_font(canvas, FontSecondary);
        for(uint8_t i = 0; i < RESULT_VISIBLE_LINES; i++) {
            uint8_t idx = m->scroll + i;
            if(idx >= m->line_count) break;
            canvas_draw_str(canvas, 2, 24 + i * 9, m->lines[idx]);
        }
        if(m->line_count > RESULT_VISIBLE_LINES) {
            elements_scrollbar_pos(
                canvas, 126, 15, 49, m->scroll, m->line_count - RESULT_VISIBLE_LINES + 1);
        }
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

static void reader_set_scanning(ReaderApp* app, const char* band, bool lf) {
    with_view_model(
        app->view,
        ReaderModel * m,
        {
            m->state = ReaderStateScanning;
            m->lf = lf;
            strncpy(m->band, band, sizeof(m->band) - 1);
            m->band[sizeof(m->band) - 1] = '\0';
        },
        true);
}

/* --------------------------- nfc callbacks -------------------------- */

// Runs on the NFC worker thread. All four base protocols use 0 = Error,
// 1 = Ready; FeliCa additionally treats Incomplete as usable.
static NfcCommand reader_poller_callback(NfcGenericEvent event, void* context) {
    ReaderApp* app = context;
    bool ready = false;

    switch(event.protocol) {
    case NfcProtocolIso14443_3a:
        ready =
            ((Iso14443_3aPollerEvent*)event.event_data)->type == Iso14443_3aPollerEventTypeReady;
        break;
    case NfcProtocolIso14443_4a:
        ready =
            ((Iso14443_4aPollerEvent*)event.event_data)->type == Iso14443_4aPollerEventTypeReady;
        break;
    case NfcProtocolIso14443_3b:
        ready =
            ((Iso14443_3bPollerEvent*)event.event_data)->type == Iso14443_3bPollerEventTypeReady;
        break;
    case NfcProtocolIso15693_3:
        ready =
            ((Iso15693_3PollerEvent*)event.event_data)->type == Iso15693_3PollerEventTypeReady;
        break;
    case NfcProtocolFelica: {
        FelicaPollerEventType t = ((FelicaPollerEvent*)event.event_data)->type;
        ready = (t == FelicaPollerEventTypeReady) || (t == FelicaPollerEventTypeIncomplete);
        break;
    }
    case NfcProtocolSt25tb:
        ready = ((St25tbPollerEvent*)event.event_data)->type == St25tbPollerEventTypeReady;
        break;
    default:
        break;
    }

    if(!ready) {
        return NfcCommandContinue; // keep polling until a card activates
    }

    const NfcDeviceData* data = nfc_poller_get_data(app->poller);
    nfc_device_set_data(app->device, event.protocol, data);

    size_t len = 0;
    const uint8_t* uid = nfc_device_get_uid(app->device, &len);
    if(len > ID_MAX_LEN) len = ID_MAX_LEN;
    memcpy(app->scratch_id, uid, len);
    app->scratch_id_len = len;

    // Run the read-only EMV chain now, while still inside the poller
    // callback: iso14443_4a_poller_send_block() is only legal here.
    memset(&app->emv, 0, sizeof(app->emv));
    if(event.protocol == NfcProtocolIso14443_4a) {
        emv_read((Iso14443_4aPoller*)event.instance, &app->emv);
    }

    FuriString* hex = furi_string_alloc();
    for(size_t i = 0; i < len; i++) {
        furi_string_cat_printf(hex, "%02X", app->scratch_id[i]);
    }
    FURI_LOG_I(
        TAG,
        "NFC read: %s, UID %s",
        nfc_device_get_protocol_name(app->display_protocol),
        furi_string_get_cstr(hex));
    furi_string_free(hex);

    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(ReaderEventNfcRead, app->gen));
    return NfcCommandStop;
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
    app->base_protocol = protocol_base(best);
    app->emv_capable = protocol_is_iso14443_4a(best);
    if(app->emv_capable) app->base_protocol = NfcProtocolIso14443_4a;
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
    reader_set_scanning(app, BAND_NFC, false);

    FURI_LOG_D(TAG, "phase: NFC (gen %lu)", (unsigned long)app->gen);
    app->scanner = nfc_scanner_alloc(app->nfc);
    nfc_scanner_start(app->scanner, reader_scanner_callback, app);
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(NFC_PHASE_MS));
}

static void reader_start_lf_phase(ReaderApp* app) {
    reader_stop_all(app);
    app->gen++;
    app->lf_phase = true;
    reader_set_scanning(app, BAND_LF, true);

    FURI_LOG_D(TAG, "phase: LF (gen %lu)", (unsigned long)app->gen);
    lfrfid_worker_start_thread(app->worker);
    app->lf_thread_running = true;
    lfrfid_worker_read_start(app->worker, LFRFIDWorkerReadTypeAuto, reader_lf_callback, app);
    app->lf_reading = true;
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(LF_PHASE_MS));
}

/* --------------------------- EMV formatting -------------------------- */

// Tag 9A, YYMMDD packed BCD. Each nibble is already a decimal digit, so
// printing the bytes in hex reproduces the decimal date; displayed DD/MM/YY.
static void format_emv_date(const uint8_t* bcd, char* out, size_t cap) {
    snprintf(out, cap, "%02X/%02X/%02X", (unsigned)bcd[2], (unsigned)bcd[1], (unsigned)bcd[0]);
}

// Tag 9F02, 6 packed-BCD bytes (12 digits, n12). Strips leading zeros down to
// a minimum of 3 digits, then inserts '.' before the last two (minor units):
// "000000000100" -> "1.00", "000000001234" -> "12.34".
static void format_emv_amount(const uint8_t* bcd, char* out, size_t cap) {
    char digits[13];
    for(size_t i = 0; i < 6; i++) {
        digits[i * 2] = (char)('0' + (bcd[i] >> 4));
        digits[i * 2 + 1] = (char)('0' + (bcd[i] & 0x0F));
    }
    digits[12] = '\0';

    size_t start = 0;
    while(start < 9 && digits[start] == '0') start++; // keep >= 3 digits
    size_t len = 12 - start;
    const char* d = digits + start;

    snprintf(out, cap, "%.*s.%.*s", (int)(len - 2), d, 2, d + len - 2);
}

// Tag 5F2A, ISO 4217 numeric. Unknown codes print as the raw number.
static const char* format_emv_currency(uint16_t code, char* fallback, size_t fallback_cap) {
    static const struct {
        uint16_t code;
        const char* name;
    } table[] = {
        {978, "EUR"},
        {826, "GBP"},
        {840, "USD"},
        {752, "SEK"},
        {578, "NOK"},
        {208, "DKK"},
        {985, "PLN"},
        {203, "CZK"},
        {348, "HUF"},
        {756, "CHF"},
    };
    for(size_t i = 0; i < COUNT_OF(table); i++) {
        if(table[i].code == code) return table[i].name;
    }
    snprintf(fallback, fallback_cap, "%u", (unsigned)code);
    return fallback;
}

/* --------------------------- view callbacks ------------------------- */

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
        app->poller = nfc_poller_alloc(app->nfc, app->base_protocol);
        nfc_poller_start(app->poller, reader_poller_callback, app);
        // Bounded read: a card removed now must not leave us stuck on "Reading".
        // EMV cards get a longer bound: the command chain runs inside the
        // poller callback, and nfc_poller_stop() joins that thread, so a
        // short timeout would block the GUI thread mid-chain.
        furi_timer_start(
            app->phase_timer,
            furi_ms_to_ticks(app->emv_capable ? EMV_READ_TIMEOUT_MS : READ_TIMEOUT_MS));
        return true;

    case ReaderEventNfcRead: {
        if(!app->poller) return true;
        reader_stop_all(app);
        app->gen++;
        const char* name = nfc_device_get_protocol_name(app->display_protocol);
        with_view_model(
            app->view,
            ReaderModel * m,
            {
                m->state = ReaderStateDone;
                m->lf = false;
                strncpy(m->band, BAND_NFC, sizeof(m->band) - 1);
                m->band[sizeof(m->band) - 1] = '\0';
                strncpy(m->type_name, name, sizeof(m->type_name) - 1);
                m->type_name[sizeof(m->type_name) - 1] = '\0';

                result_reset(m);
                result_addf(m, "Band: 13.56 MHz");
                result_addf(m, "Type: %s", name);
                result_add_hex(m, "UID:", app->scratch_id, app->scratch_id_len);

                if(app->emv.ppse_ok) {
                    for(uint8_t i = 0; i < app->emv.aid_count; i++) {
                        result_add_hex(
                            m, i == 0 ? "AID:" : "AID+", app->emv.aid[i], app->emv.aid_len[i]);
                    }
                    if(app->emv.label[0]) result_addf(m, "App: %s", app->emv.label);

                    if(app->emv.pan[0]) {
                        result_addf(m, "PAN: %s", app->emv.pan);
                    } else {
                        result_addf(m, "PAN: not available");
                        result_addf(m, "over contactless");
                    }

                    if(app->emv.expiry[0]) {
                        result_addf(m, "Expiry: %s", app->emv.expiry);
                    } else {
                        result_addf(m, "Expiry: not disclosed");
                    }

                    if(app->emv.name[0]) {
                        result_addf(m, "Name: %s", app->emv.name);
                    } else {
                        result_addf(m, "Name: not disclosed");
                    }

                    if(app->emv.log_count) {
                        result_addf(m, "Txn log: %u records", (unsigned)app->emv.log_count);
                        for(uint8_t r = 0; r < app->emv.log_rows; r++) {
                            const EmvLogRow* row = &app->emv.log[r];
                            char date_str[12];
                            char amount_str[16];
                            char cur_fallback[8];
                            const char* cur_str = "--";
                            if(row->has_date) {
                                format_emv_date(row->date, date_str, sizeof(date_str));
                            } else {
                                snprintf(date_str, sizeof(date_str), "--/--/--");
                            }
                            if(row->has_amount) {
                                format_emv_amount(row->amount, amount_str, sizeof(amount_str));
                            } else {
                                snprintf(amount_str, sizeof(amount_str), "--");
                            }
                            if(row->has_currency) {
                                cur_str = format_emv_currency(
                                    row->currency, cur_fallback, sizeof(cur_fallback));
                            }
                            result_addf(m, "%s %s %s", date_str, amount_str, cur_str);
                        }
                        if(app->emv.log_rows == 0) result_addf(m, "(log format not given)");
                    }
                } else if(app->emv_capable) {
                    result_addf(m, "No EMV app on card");
                }
            },
            true);
        return true;
    }

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

        with_view_model(
            app->view,
            ReaderModel * m,
            {
                m->state = ReaderStateDone;
                m->lf = true;
                strncpy(m->band, BAND_LF, sizeof(m->band) - 1);
                m->band[sizeof(m->band) - 1] = '\0';
                strncpy(m->type_name, name ? name : "Unknown", sizeof(m->type_name) - 1);
                m->type_name[sizeof(m->type_name) - 1] = '\0';

                result_reset(m);
                result_addf(m, "Band: 125 kHz");
                result_addf(m, "Type: %s", name ? name : "Unknown");
                result_add_hex(m, "ID:", app->scratch_id, app->scratch_id_len);
            },
            true);
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

    if(state == ReaderStateDone && (event->key == InputKeyUp || event->key == InputKeyDown)) {
        with_view_model(
            app->view,
            ReaderModel * m,
            {
                if(event->key == InputKeyDown) {
                    if(m->line_count > RESULT_VISIBLE_LINES &&
                       m->scroll < m->line_count - RESULT_VISIBLE_LINES)
                        m->scroll++;
                } else if(m->scroll > 0) {
                    m->scroll--;
                }
            },
            true);
        return true;
    }

    if(event->key == InputKeyOk && (state == ReaderStateDone || state == ReaderStateError)) {
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
    view_dispatcher_add_view(app->view_dispatcher, 0, app->view);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

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

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_free(app->view);
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
