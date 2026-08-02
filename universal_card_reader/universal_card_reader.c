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

#include <input/input.h>

#include <lfrfid/lfrfid_dict_file.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <storage/storage.h>

#include "reader_app.h"
#include "reader_ui.h"
#include "reader_nfc.h"
#include "reader_lf.h"

/* ----------------------------- helpers ------------------------------ */

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

// Contiguous uppercase hex, no separator: used for save-file stems and the LF read log.
void reader_cat_hex(FuriString* out, const uint8_t* data, size_t len) {
    for(size_t i = 0; i < len; i++) furi_string_cat_printf(out, "%02X", data[i]);
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
    reader_cat_hex(out, id, id_len);
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

/* --------------------------- phase lifecycle ------------------------ */

// Tear down whatever phase is running and cancel a pending phase timeout.
void reader_stop_all(ReaderApp* app) {
    furi_timer_stop(app->phase_timer);
    app->notice_active = false;
    reader_stop_nfc(app);
    reader_stop_lf(app);
}

// The only place that changes views. Keeps current_view (which the navigation
// callback reads) and the animation timer in sync: only the scan view animates,
// so a timer left running on a static screen would post a tick into the
// dispatcher every ANIM_PERIOD_MS for nothing.
void reader_switch_view(ReaderApp* app, ReaderView view) {
    app->current_view = view;
    if(view == ReaderViewScan) {
        furi_timer_start(app->anim_timer, furi_ms_to_ticks(ANIM_PERIOD_MS));
    } else {
        furi_timer_stop(app->anim_timer);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

// GUI thread only. Shows a message on the status view (reusing ReaderViewScan)
// and auto-returns to the report after NOTICE_MS. Used for save results and
// blocked-action explanations; callers stop their own hardware before calling
// this, so it never touches a radio itself.
void reader_show_notice(ReaderApp* app, const char* title, const char* l1, const char* l2) {
    reader_stop_all(app);
    app->gen++;
    app->notice_active = true;
    reader_set_notice(app, title, l1, l2);
    reader_switch_view(app, ReaderViewScan);
    furi_timer_stop(app->anim_timer); // the notice screen is static, unlike Emulating
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(NOTICE_MS));
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

static void reader_handle_phase_timeout(ReaderApp* app) {
    // Either the band was empty, or a card was pulled away mid-read.
    if(app->poller) FURI_LOG_I(TAG, "read timed out, resuming scan");
    if(app->lf_phase) {
        reader_start_nfc_phase(app);
    } else {
        reader_start_lf_phase(app);
    }
}

// Unreachable today — see the TODO on ReaderEventError in reader_app.h.
static void reader_handle_error(ReaderApp* app) {
    reader_stop_all(app);
    app->gen++;
    furi_timer_stop(app->anim_timer); // the error screen is static
    reader_set_state(app, ReaderStateError);
}

static void reader_handle_exit(ReaderApp* app) {
    reader_stop_all(app);
    app->gen++;
    view_dispatcher_stop(app->view_dispatcher);
}

static bool reader_custom_event_callback(void* context, uint32_t event) {
    ReaderApp* app = context;

    if(event == ReaderEventAnimTick) {
        reader_bump_frame(app);
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
    case ReaderEventPhaseTimeout:  reader_handle_phase_timeout(app);        return true;
    case ReaderEventNfcScanned:    reader_nfc_handle_scanned(app);          return true;
    case ReaderEventNfcRead:       reader_nfc_handle_read(app);             return true;
    case ReaderEventLfRead:        reader_lf_handle_read(app);              return true;
    case ReaderEventError:         reader_handle_error(app);                return true;
    case ReaderEventActionSave:    reader_do_save(app);                     return true;
    case ReaderEventActionEmulate: reader_do_emulate(app);                  return true;
    case ReaderEventActionRescan:  reader_start_nfc_phase(app);             return true;
    case ReaderEventActionExit:    reader_handle_exit(app);                 return true;
    case ReaderEventNoticeDone:    reader_switch_view(app, ReaderViewInfo); return true;
    default:                       return false;
    }
}

static bool reader_input_callback(InputEvent* event, void* context) {
    ReaderApp* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    ReaderState state;
    state = reader_get_state(app);

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
