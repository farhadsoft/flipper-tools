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

#include <dialogs/dialogs.h>
#include <lfrfid/lfrfid_dict_file.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <storage/storage.h>

#include "reader_app.h"
#include "reader_ui.h"
#include "reader_nfc.h"
#include "reader_lf.h"
#include "card_info.h"

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

// storage_simply_mkdir() creates one level and returns true when the path
// already exists, so every parent is created explicitly. Called from both
// reader_do_save() and reader_do_load(): the file browser silently walks up
// to /ext when its base_path is missing, so this is correctness, not hygiene.
static void reader_ensure_dirs(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(storage, READER_SAVE_DIR);
    furi_record_close(RECORD_STORAGE);
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
void reader_show_notice(
    ReaderApp* app, const char* title, const char* l1, const char* l2, ReaderView back_to) {
    app->notice_return = back_to;
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

    // The file-actions Submenu does not consume Back, same as the actions menu.
    // Routed through the dispatcher because reader_load_abort() may restart the
    // NFC phase, and radio starts belong in reader_custom_event_callback().
    if(app->current_view == ReaderViewFileMenu) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, EVENT_MAKE(ReaderEventFileBack, app->gen));
        return true;
    }

    // TextInput consumes long/repeat Back as backspace but not a short press
    // (verified in the firmware's text_input.c), so a short Back lands here.
    if(app->current_view == ReaderViewRename) {
        reader_switch_view(app, ReaderViewFileMenu);
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
    if(app->card == ReaderCardNone) {
        reader_show_notice(app, "No card", "read or load one", "", ReaderViewInfo);
        return;
    }
    if(app->card == ReaderCardEmvFile) {
        reader_show_notice(app, "From file", "already saved", "", ReaderViewInfo);
        return;
    }

    reader_ensure_dirs();

    FuriString* path = furi_string_alloc();
    bool ok = false;

    if(app->card == ReaderCardLf) {
        const char* name = protocol_dict_get_name(app->dict, app->lf_protocol);
        reader_build_path(
            path, READER_SAVE_DIR, name ? name : "Unknown", app->scratch_id,
            app->scratch_id_len, ".rfid");
        ok = lfrfid_dict_file_save(app->dict, app->lf_protocol, furi_string_get_cstr(path));
    } else if(reader_is_payment_card(app)) {
        // EMV / bank card: save ALL data (PAN, expiry, name, AIDs, track2, log)
        // to a dedicated .emv file, plus the ISO14443-4A transport (UID/ATS)
        // so the saved file can be emulated after a Load.
        size_t uid_len = 0;
        const uint8_t* uid = nfc_device_get_uid(app->device, &uid_len);
        reader_build_path(path, READER_SAVE_DIR, "EMV", uid, uid_len, ".emv");
        ok = emv_save(&app->emv, app->device, furi_string_get_cstr(path));
    } else {
        size_t uid_len = 0;
        const uint8_t* uid = nfc_device_get_uid(app->device, &uid_len);
        reader_build_path(
            path, READER_SAVE_DIR, nfc_device_get_protocol_name(app->display_protocol), uid,
            uid_len, ".nfc");
        ok = nfc_device_save(app->device, furi_string_get_cstr(path));
    }

    const char* full = furi_string_get_cstr(path);
    const char* base = strrchr(full, '/');
    base = base ? base + 1 : full;
    FURI_LOG_I(TAG, "save %s: %s", ok ? "ok" : "FAILED", full);
    reader_show_notice(app, ok ? "Saved" : "Save failed", READER_SAVE_DIR_UI, base, ReaderViewInfo);
    furi_string_free(path);
}

static bool reader_load_nfc_file(ReaderApp* app, const char* full) {
    bool ok = nfc_device_load(app->device, full);
    if(ok) {
        memset(&app->emv, 0, sizeof(app->emv)); // no stale bank data from a previous card
        // display_protocol comes from the firmware's own device table, so it may be
        // a fork-only protocol (Momentum NfcProtocolEmv/Ntag4xx/Type4Tag) for a file
        // dropped in over USB - safe here since it is only used for the name/chain
        // (card_info's dev_has()) and as input to reader_poll_protocol(), which
        // resolves it via nfc_protocol_has_parent() (firmware-evaluated) to the same
        // compile-time-whitelisted id a live scan would have produced. Never compare
        // display_protocol itself against a sentinel.
        app->display_protocol = nfc_device_get_protocol(app->device);
        app->poll_protocol = reader_poll_protocol(app->display_protocol);
        app->card = ReaderCardNfc;
        reader_report_begin(app);
        card_info_format_nfc(app->info_text, app->device, app->display_protocol, &app->emv);
        reader_report_show(app, nfc_device_get_protocol_name(app->display_protocol));
    }
    return ok;
}

static bool reader_load_emv_file(ReaderApp* app, const char* full) {
    bool ok = emv_load(&app->emv, app->device, full);
    if(ok) {
        app->card = ReaderCardEmvFile;
        if(app->emv.has_transport) {
            // v3 file: the 4A transport was restored into the device, so
            // Emulate can run exactly like a live ISO14443-4A read.
            app->display_protocol = NfcProtocolIso14443_4a;
            app->poll_protocol = NfcProtocolIso14443_4a;
        }
        reader_report_begin(app);
        card_info_format_emv(app->info_text, &app->emv);
        reader_report_show(app, "EMV file");
    }
    return ok;
}

static bool reader_load_rfid_file(ReaderApp* app, const char* full) {
    ProtocolId id = lfrfid_dict_file_load(app->dict, full);
    bool ok = (id != PROTOCOL_NO);
    if(ok) {
        app->lf_protocol = id;
        size_t size = protocol_dict_get_data_size(app->dict, id);
        if(size > ID_MAX_LEN) size = ID_MAX_LEN;
        protocol_dict_get_data(app->dict, id, app->scratch_id, size);
        app->scratch_id_len = size;
        app->card = ReaderCardLf;
        const char* name = protocol_dict_get_name(app->dict, id);
        reader_report_begin(app);
        card_info_format_lf(
            app->info_text, name ? name : "Unknown", app->scratch_id, app->scratch_id_len);
        reader_report_show(app, name ? name : "Unknown");
    }
    return ok;
}

// Where Load lands when nothing was opened: back to scanning if Load was entered
// with OK from the scan screen, otherwise to the report the user left. Shared by
// the browser-cancel path and the file menu's Back.
static void reader_load_abort(ReaderApp* app) {
    if(app->load_from_scan) {
        reader_start_nfc_phase(app);
    } else {
        reader_switch_view(app, ReaderViewInfo);
    }
}

// The extension picks the loader, which is why Rename preserves it.
static void reader_open_selected(ReaderApp* app) {
    const char* full = furi_string_get_cstr(app->selected_path);
    bool ok = false;

    if(furi_string_end_with_str(app->selected_path, ".nfc")) {
        ok = reader_load_nfc_file(app, full);
    } else if(furi_string_end_with_str(app->selected_path, ".emv")) {
        ok = reader_load_emv_file(app, full);
    } else if(furi_string_end_with_str(app->selected_path, ".rfid")) {
        ok = reader_load_rfid_file(app, full);
    }

    FURI_LOG_I(TAG, "load %s: %s", ok ? "ok" : "FAILED", full);
    if(!ok) {
        const char* base = strrchr(full, '/');
        reader_show_notice(
            app, "Load failed", base ? base + 1 : full, "", ReaderViewFileMenu);
    }
}

// GUI thread only. dialog_file_browser_show() blocks this thread until the
// user picks or cancels, so every radio must be down and — critically — the
// animation timer must be stopped first: view_dispatcher_send_custom_event()
// blocks FuriWaitForever on a 16-deep queue, so an 80 ms tick left running
// fills it in ~1.3 s and then blocks the TimersSrv thread for the whole
// dialog. See invariant 6 in CLAUDE.md.
static void reader_do_load(ReaderApp* app) {
    app->load_from_scan = (app->current_view == ReaderViewScan);

    reader_stop_all(app);
    app->gen++;
    furi_timer_stop(app->anim_timer);
    reader_ensure_dirs();

    FuriString* path = furi_string_alloc_set_str(READER_SAVE_DIR);
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, "*", NULL); // initialises every field
    opts.base_path = READER_SAVE_DIR;
    opts.hide_ext = false; // the extension picks the loader; show it

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    bool picked = dialog_file_browser_show(dialogs, path, path, &opts);
    furi_record_close(RECORD_DIALOGS);

    if(!picked) {
        furi_string_free(path);
        reader_load_abort(app);
        return;
    }

    furi_string_set(app->selected_path, path);
    furi_string_free(path);

    const char* full = furi_string_get_cstr(app->selected_path);
    const char* base = strrchr(full, '/');
    submenu_set_header(app->file_menu, base ? base + 1 : full); // copies the string
    submenu_set_selected_item(app->file_menu, 0); // never land on Delete
    reader_switch_view(app, ReaderViewFileMenu);
}

static void reader_do_delete(ReaderApp* app) {
    const char* path = furi_string_get_cstr(app->selected_path);
    const char* base = strrchr(path, '/');

    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool ok = storage_simply_remove(storage, path);
    furi_record_close(RECORD_STORAGE);

    if(!ok) {
        // false is a real failure: storage_simply_remove() returns true when the
        // item is already gone (storage.h). Reporting "Deleted" on a failed
        // remove is a lie the user acts on.
        FURI_LOG_E(TAG, "delete failed: %s", path);
        reader_show_notice(
            app, "Delete failed", base ? base + 1 : path, "", ReaderViewFileMenu);
        return;
    }

    FURI_LOG_I(TAG, "deleted %s", path);
    // reader_set_notice() snprintf-copies the name, so the notice survives the
    // reset; the file menu would show a stale header, so land on the actions menu.
    reader_show_notice(app, "Deleted", base ? base + 1 : path, "", ReaderViewActions);
    furi_string_reset(app->selected_path); // nothing may act on a file that is gone
}

// Runs on the GUI thread (TextInput input path) and starts no radio, so it acts
// directly instead of posting an event — same licence reader_navigation_callback()
// takes.
static void reader_rename_result(void* context) {
    ReaderApp* app = context;

    // The keyboard can emit spaces (shifted '_') ; reuse Save's stem rule so
    // renamed files look like generated ones. In-place is safe: same-index copy.
    reader_sanitize(app->rename_buf, sizeof(app->rename_buf), app->rename_buf);
    if(app->rename_buf[0] == '\0') {
        reader_show_notice(app, "Rename failed", "empty name", "", ReaderViewFileMenu);
        return;
    }

    const char* old_path = furi_string_get_cstr(app->selected_path);
    const char* old_base = strrchr(old_path, '/');
    old_base = old_base ? old_base + 1 : old_path;
    const char* dot = strrchr(old_base, '.');
    const char* ext = dot ? dot : ""; // ".nfc" / ".emv" / ".rfid", or none
    size_t stem_len = strlen(app->rename_buf);
    size_t ext_len = strlen(ext);
    // Typing "card.nfc" must not produce "card.nfc.nfc".
    bool typed_ext = ext_len && stem_len >= ext_len &&
                     strcmp(app->rename_buf + stem_len - ext_len, ext) == 0;

    // Same directory as the picked file (the browser can descend into a subdir
    // a user created over USB), same extension, new stem.
    FuriString* new_path = furi_string_alloc();
    furi_string_set_n(new_path, app->selected_path, 0, (size_t)(old_base - old_path));
    furi_string_cat_str(new_path, app->rename_buf);
    if(!typed_ext) furi_string_cat_str(new_path, ext);

    const char* fail = NULL;
    if(furi_string_cmp(new_path, app->selected_path) != 0) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        if(storage_file_exists(storage, furi_string_get_cstr(new_path))) {
            // storage_common_rename() overwrites the destination silently
            // (storage.h) — that would destroy another saved card.
            fail = "name already used";
        } else {
            FS_Error err = storage_common_rename(
                storage, old_path, furi_string_get_cstr(new_path));
            if(err != FSE_OK) fail = storage_error_get_desc(err);
        }
        furi_record_close(RECORD_STORAGE);
    }

    if(fail) {
        FURI_LOG_E(
            TAG, "rename failed: %s -> %s (%s)", old_path,
            furi_string_get_cstr(new_path), fail);
        furi_string_free(new_path);
        reader_show_notice(app, "Rename failed", fail, "", ReaderViewFileMenu);
        return;
    }

    // old_path / old_base / ext all point into selected_path and die here.
    furi_string_set(app->selected_path, new_path);
    furi_string_free(new_path);

    const char* full = furi_string_get_cstr(app->selected_path);
    const char* base = strrchr(full, '/');
    base = base ? base + 1 : full;
    FURI_LOG_I(TAG, "renamed to %s", full);
    submenu_set_header(app->file_menu, base);
    reader_show_notice(app, "Renamed", base, "", ReaderViewFileMenu);
}

static void reader_do_rename_start(ReaderApp* app) {
    const char* path = furi_string_get_cstr(app->selected_path);
    const char* base = strrchr(path, '/');

    snprintf(app->rename_buf, sizeof(app->rename_buf), "%s", base ? base + 1 : path);
    char* dot = strrchr(app->rename_buf, '.');
    if(dot) *dot = '\0'; // edit the stem only; the extension picks the loader

    text_input_set_header_text(app->rename_input, "New name");
    text_input_set_result_callback(
        app->rename_input, reader_rename_result, app, app->rename_buf,
        sizeof(app->rename_buf), false);
    reader_switch_view(app, ReaderViewRename);
}

static void reader_do_emulate(ReaderApp* app) {
    if(app->card == ReaderCardNone) {
        reader_show_notice(app, "No card", "read or load one", "", ReaderViewInfo);
        return;
    }
    if(app->card == ReaderCardEmvFile) {
        // .emv v2 files (and v3 files whose transport block failed to parse)
        // carry no UID/ATS, so the listener would have nothing to present.
        if(!app->emv.has_transport) {
            reader_show_notice(app, "Blocked", "no transport data", "in .emv file", ReaderViewInfo);
            return;
        }
        reader_start_nfc_emulation(app);
        return;
    }

    if(app->card == ReaderCardLf) {
        reader_start_lf_emulation(app);
        return;
    }
    // EMV / bank card: emulate at ISO14443-4A level (UID + ATS).
    // The card presents its full data (PAN, expiry, AIDs, track2, log) to any
    // reader that queries it, just like the original card.
    if(reader_is_payment_card(app)) {
        reader_start_nfc_emulation(app);
        return;
    }
    if(!reader_protocol_emulatable(app->poll_protocol)) {
        reader_show_notice(
            app, "Blocked", "No emulation for", nfc_device_get_protocol_name(app->poll_protocol),
            ReaderViewInfo);
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
    case ReaderEventActionSave:    reader_do_save(app);                     return true;
    case ReaderEventActionEmulate: reader_do_emulate(app);                  return true;
    case ReaderEventActionRescan:  reader_start_nfc_phase(app);             return true;
    case ReaderEventActionLoad:    reader_do_load(app);                     return true;
    case ReaderEventActionExit:    reader_handle_exit(app);                 return true;
    case ReaderEventNoticeDone:    reader_switch_view(app, app->notice_return); return true;
    case ReaderEventFileOpen:      reader_open_selected(app);               return true;
    case ReaderEventFileRename:    reader_do_rename_start(app);             return true;
    case ReaderEventFileDelete:    reader_do_delete(app);                   return true;
    case ReaderEventFileBack:      reader_load_abort(app);                  return true;
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
            reader_switch_view(app, app->notice_return);
            return true;
        }
        return true; // swallow everything else while the notice is up
    }

    if(event->key == InputKeyOk && state == ReaderStateScanning) {
        // Routed through the dispatcher like every other action so the radio
        // teardown stays inside reader_custom_event_callback().
        view_dispatcher_send_custom_event(
            app->view_dispatcher, EVENT_MAKE(ReaderEventActionLoad, app->gen));
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
    furi_string_reserve(app->info_text, CARD_INFO_MAX);
    furi_string_set_str(app->info_text, "No card loaded.\n\nOK on the scan screen\nopens saved cards.\n");
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->info_text));

    app->actions = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ReaderViewActions, submenu_get_view(app->actions));
    submenu_add_item(app->actions, "Save", ReaderEventActionSave, reader_action_callback, app);
    submenu_add_item(
        app->actions, "Emulate", ReaderEventActionEmulate, reader_action_callback, app);
    submenu_add_item(
        app->actions, "Rescan", ReaderEventActionRescan, reader_action_callback, app);
    submenu_add_item(app->actions, "Load", ReaderEventActionLoad, reader_action_callback, app);
    submenu_add_item(app->actions, "Exit", ReaderEventActionExit, reader_action_callback, app);

    app->file_menu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ReaderViewFileMenu, submenu_get_view(app->file_menu));
    submenu_add_item(app->file_menu, "Open", ReaderEventFileOpen, reader_action_callback, app);
    submenu_add_item(
        app->file_menu, "Rename", ReaderEventFileRename, reader_action_callback, app);
    submenu_add_item(
        app->file_menu, "Delete", ReaderEventFileDelete, reader_action_callback, app);
    submenu_add_item(app->file_menu, "Back", ReaderEventFileBack, reader_action_callback, app);

    app->rename_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ReaderViewRename, text_input_get_view(app->rename_input));
    app->selected_path = furi_string_alloc();

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
    view_dispatcher_remove_view(app->view_dispatcher, ReaderViewFileMenu);
    submenu_free(app->file_menu);
    view_dispatcher_remove_view(app->view_dispatcher, ReaderViewRename);
    text_input_free(app->rename_input);
    furi_string_free(app->selected_path);
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
