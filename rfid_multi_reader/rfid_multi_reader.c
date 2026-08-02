/*
 * RFID Multi-Reader for Flipper Zero
 * ------------------------------------------------------------
 * Universal read-only RFID reader: 125 kHz LF and 13.56 MHz HF/NFC, both
 * built into the Flipper, plus a UHF (860-960 MHz) menu entry that explains
 * an external module is required. No save, no emulation - read + display
 * only.
 *
 * Each band is a plugin behind the RfidBackend vtable (rfid_backend.h); this
 * file never touches the nfc_ / lfrfid_worker_ APIs directly. All backend
 * calls happen on the GUI thread, driven from the custom-event router below.
 * Worker callbacks and the phase/read/notice timer only ever signal via
 * view_dispatcher_send_custom_event() - see the generation-stamping note in
 * rfid_app.h.
 *
 * Legitimate use only: read cards/tags you own or are authorised to test.
 *
 * Target: Flipper Zero official firmware 1.x / SDK API 87.1. Build with ufbt.
 */

#include "rfid_app.h"
#include "ui.h"
#include "backend_hf.h"
#include "backend_lf.h"
#include "backend_uhf.h"

/* ----------------------------- helpers -------------------------------- */

static const char* rfid_mode_label(RfidMode mode) {
    switch(mode) {
    case RfidModeAuto: return "Auto";
    case RfidModeHf: return "HF only";
    case RfidModeLf: return "LF only";
    case RfidModeUhf: return "UHF"; // unreachable while scanning
    default: return "";
    }
}

// Fills rotation[]/rotation_len from mode, skipping any band the hardware
// cannot drive. rot_idx always restarts at the first entry.
static void rfid_build_rotation(RfidApp* app) {
    app->rotation_len = 0;
    app->rot_idx = 0;

    RfidBackend* hf = app->backends[RfidBandHf];
    RfidBackend* lf = app->backends[RfidBandLf];
    RfidBackend* uhf = app->backends[RfidBandUhf];

    switch(app->mode) {
    case RfidModeAuto:
        if(hf && hf->available()) app->rotation[app->rotation_len++] = hf;
        if(lf && lf->available()) app->rotation[app->rotation_len++] = lf;
        break;
    case RfidModeHf:
        if(hf && hf->available()) app->rotation[app->rotation_len++] = hf;
        break;
    case RfidModeLf:
        if(lf && lf->available()) app->rotation[app->rotation_len++] = lf;
        break;
    case RfidModeUhf: // unreachable: RfidEventMenuUhf never sets app->mode
        if(uhf && uhf->available()) app->rotation[app->rotation_len++] = uhf;
        break;
    }
}

/* --------------------------- phase lifecycle --------------------------- */

// Tear down whatever is running on EVERY backend, not just the active one -
// the one guarantee that two radios can never be up at once. Idempotent.
void rfid_stop_all(RfidApp* app) {
    furi_timer_stop(app->timer);
    app->timer_role = RfidTimerNone;
    for(size_t i = 0; i < RfidBandCount; i++) {
        if(app->backends[i]) app->backends[i]->scan_stop(app->backends[i]);
    }
}

// The only place that changes views. Keeps current_view (which the
// navigation callback reads) and the animation timer in sync: only the
// status view animates, so a timer left running on a static screen would
// post a tick into the dispatcher every ANIM_PERIOD_MS for nothing.
void rfid_switch_view(RfidApp* app, RfidView view) {
    app->current_view = view;
    if(view == RfidViewStatus) {
        furi_timer_start(app->anim_timer, furi_ms_to_ticks(ANIM_PERIOD_MS));
    } else {
        furi_timer_stop(app->anim_timer);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

// GUI thread only. Shows a message on the status view and auto-returns to
// the menu after NOTICE_MS. Callers stop their own hardware before calling
// this, so it never touches a radio itself.
void rfid_show_notice(RfidApp* app, const char* title, const char* l1, const char* l2) {
    rfid_stop_all(app);
    app->gen++;
    app->active = NULL;
    app->timer_role = RfidTimerNotice;
    rfid_set_notice(app, title, l1, l2);
    rfid_set_state(app, RfidStateNotice);
    rfid_switch_view(app, RfidViewStatus);
    furi_timer_stop(app->anim_timer); // the notice screen is static, unlike Scanning
    furi_timer_start(app->timer, furi_ms_to_ticks(NOTICE_MS));
}

// Worker-thread bridges - the only things a backend ever calls back into.
// Fired from a radio worker thread; do nothing but push a custom event.
static void rfid_on_detect(void* ctx) {
    RfidApp* app = ctx;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(RfidEventDetected, app->gen));
}

static void rfid_on_read(void* ctx) {
    RfidApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(RfidEventRead, app->gen));
}

void rfid_start_scan(RfidApp* app) {
    rfid_stop_all(app);
    app->gen++;
    app->active = app->rotation[app->rot_idx];
    rfid_set_scanning(app, app->active->band_label, rfid_mode_label(app->mode));
    rfid_set_state(app, RfidStateScanning);
    rfid_switch_view(app, RfidViewStatus);
    FURI_LOG_D(TAG, "phase: %s (gen %lu)", app->active->name, (unsigned long)app->gen);
    app->active->scan_start(app->active, rfid_on_detect, app);
    if(app->rotation_len > 1) {
        app->timer_role = RfidTimerPhase;
        furi_timer_start(app->timer, furi_ms_to_ticks(app->active->scan_ms));
    }
    // Single-band modes arm no phase timer: nothing to rotate to, so the scan
    // runs until a card appears or Back is pressed. The read timeout below is
    // still armed in every mode.
}

void rfid_advance_phase(RfidApp* app) {
    app->rot_idx = (app->rot_idx + 1) % app->rotation_len;
    rfid_start_scan(app);
}

// Menu row selection: Auto/HF-only/LF-only all funnel through here.
static void rfid_select_mode(RfidApp* app, RfidMode mode) {
    app->mode = mode;
    rfid_build_rotation(app);
    if(app->rotation_len == 0) {
        rfid_show_notice(app, "Unavailable", "No hardware for", "this band");
    } else {
        rfid_start_scan(app);
    }
}

static void rfid_return_to_menu(RfidApp* app) {
    rfid_stop_all(app);
    app->gen++;
    app->active = NULL;
    rfid_set_state(app, RfidStateIdle);
    // submenu_set_selected_item() takes the row POSITION, not the item's
    // registered index/event id - RfidMode is declared in the same order the
    // rows are added, so the cast reopens the menu on the band last picked.
    submenu_set_selected_item(app->menu, (uint32_t)app->mode);
    rfid_switch_view(app, RfidViewMenu);
}

/* ---------------------------- timer callbacks --------------------------- */

// Runs on the TimersSrv thread, not the GUI thread - reads only timer_role
// and gen, never the view model (with_view_model() from here would deadlock
// against furi_timer_start() on the GUI thread; see rfid_app.h).
static void rfid_timer_callback(void* context) {
    RfidApp* app = context;
    uint8_t id;
    switch(app->timer_role) {
    case RfidTimerPhase:
        id = RfidEventPhaseTimeout;
        break;
    case RfidTimerRead:
        id = RfidEventReadTimeout;
        break;
    case RfidTimerNotice:
        id = RfidEventNoticeDone;
        break;
    default:
        return;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(id, app->gen));
}

static void rfid_anim_timer_callback(void* context) {
    RfidApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, RfidEventAnimTick);
}

/* --------------------------- view callbacks ----------------------------- */

// Back that no view consumed. Runs on the GUI thread (input path), so
// starting a phase or switching views here is legal.
//
// This is the dispatcher's navigation callback rather than a module view's
// previous_callback: view_previous() passes view->context, and
// text_box_alloc()/submenu_alloc() set that to the TextBox/Submenu itself,
// so a previous_callback would be handed that pointer to use as an
// RfidApp* - a real device crash in universal_card_reader. The dispatcher
// passes event_context, i.e. the app, so every view transition funnels
// through here and rfid_switch_view() instead.
static bool rfid_navigation_callback(void* context) {
    RfidApp* app = context;

    if(app->current_view == RfidViewInfo || app->current_view == RfidViewStatus) {
        rfid_return_to_menu(app);
        return true;
    }
    rfid_stop_all(app); // on the menu: leave
    app->gen++;
    return false; // dispatcher stops, run() returns
}

// Band menu item callback. Runs on the GUI thread, but routes through the
// dispatcher anyway so that every radio start/stop stays inside
// rfid_custom_event_callback() (project invariant). `index` is the
// RfidCustomEvent value the row was registered with.
static void rfid_menu_callback(void* context, uint32_t index) {
    RfidApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(index, app->gen));
}

static bool rfid_custom_event_callback(void* context, uint32_t event) {
    RfidApp* app = context;

    if(event == RfidEventAnimTick) {
        rfid_bump_frame(app);
        return true;
    }

    // Anything produced by a previous phase is stale - see EVENT_MAKE above.
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
    case RfidEventPhaseTimeout:
        rfid_advance_phase(app);
        return true;

    case RfidEventReadTimeout:
        FURI_LOG_I(TAG, "read timed out, resuming scan");
        rfid_advance_phase(app);
        return true;

    case RfidEventDetected:
        // The NFC scanner re-detects in a loop, so a duplicate Detected can
        // be posted after the gen++ below and would otherwise still pass the
        // gen check and start a second poller -> furi_check in nfc_start().
        if(app->state != RfidStateScanning) return true;
        furi_timer_stop(app->timer);
        app->timer_role = RfidTimerNone;
        rfid_set_state(app, RfidStateReading);
        app->gen++;
        app->active->read(app->active, rfid_on_read, app);
        {
            uint32_t t = app->active->read_timeout_ms(app->active);
            if(t) {
                app->timer_role = RfidTimerRead;
                furi_timer_start(app->timer, furi_ms_to_ticks(t));
            }
        }
        return true;

    case RfidEventRead:
        if(app->state != RfidStateReading) return true;
        rfid_stop_all(app);
        app->gen++;
        rfid_report_begin(app);
        app->active->describe(app->active, app->info_text);
        rfid_set_state(app, RfidStateResult);
        rfid_report_show(app, app->active->card_name(app->active));
        return true;

    case RfidEventNoticeDone:
        rfid_return_to_menu(app);
        return true;

    case RfidEventMenuAuto:
        rfid_select_mode(app, RfidModeAuto);
        return true;

    case RfidEventMenuHf:
        rfid_select_mode(app, RfidModeHf);
        return true;

    case RfidEventMenuLf:
        rfid_select_mode(app, RfidModeLf);
        return true;

    case RfidEventMenuUhf:
        rfid_show_notice(
            app, "UHF 860-960 MHz", "External module required", "not built into Flipper");
        return true;

    default:
        return false;
    }
}

/* ------------------------------ app life -------------------------------- */

static RfidApp* rfid_app_alloc(void) {
    RfidApp* app = malloc(sizeof(RfidApp));
    memset(app, 0, sizeof(RfidApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->status = view_alloc();

    view_allocate_model(app->status, ViewModelTypeLocking, sizeof(RfidModel));
    view_set_context(app->status, app);
    view_set_draw_callback(app->status, rfid_draw_callback);
    // No input callback: view_input() then returns false, and
    // view_dispatcher_handle_input() routes unconsumed Back to
    // view_previous() first and only then to rfid_navigation_callback() -
    // see the note in rfid_app.h. No view here ever gets a
    // view_set_previous_callback() either, for the same reason documented
    // on rfid_navigation_callback().

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, rfid_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, rfid_navigation_callback);
    view_dispatcher_add_view(app->view_dispatcher, RfidViewStatus, app->status);

    app->text_box = text_box_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, RfidViewInfo, text_box_get_view(app->text_box));
    // text_box_set_text() stores the raw pointer, so this string must stay
    // alive and unmodified while the info view is shown.
    app->info_text = furi_string_alloc();
    furi_string_reserve(app->info_text, 8192);

    app->menu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, RfidViewMenu, submenu_get_view(app->menu));
    submenu_set_header(app->menu, "Select band");
    submenu_add_item(app->menu, "Auto (HF + LF)", RfidEventMenuAuto, rfid_menu_callback, app);
    submenu_add_item(app->menu, "13.56 MHz HF only", RfidEventMenuHf, rfid_menu_callback, app);
    submenu_add_item(app->menu, "125 kHz LF only", RfidEventMenuLf, rfid_menu_callback, app);
    submenu_add_item(app->menu, "UHF 860-960 [ext]", RfidEventMenuUhf, rfid_menu_callback, app);

    app->timer = furi_timer_alloc(rfid_timer_callback, FuriTimerTypeOnce, app);
    app->anim_timer = furi_timer_alloc(rfid_anim_timer_callback, FuriTimerTypePeriodic, app);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Table of backends this build knows about. Steps 2/3/5 each add one
    // assignment here as their backend_*.c is written; every entry an
    // unwritten step left NULL is simply skipped below and by
    // rfid_build_rotation()'s available() checks.
    app->backends[RfidBandHf] = rfid_backend_hf();
    app->backends[RfidBandLf] = rfid_backend_lf();
    app->backends[RfidBandUhf] = rfid_backend_uhf();
    for(size_t i = 0; i < RfidBandCount; i++) {
        if(app->backends[i]) app->backends[i]->alloc(app->backends[i]);
    }

    return app;
}

static void rfid_app_free(RfidApp* app) {
    // Silence the timers first so nothing can post into a dispatcher we are
    // about to tear down, then release the radios.
    furi_timer_stop(app->anim_timer);
    rfid_stop_all(app);

    // rfid_stop_all() above already joined any worker thread that was running.
    for(size_t i = 0; i < RfidBandCount; i++) {
        if(app->backends[i]) app->backends[i]->release(app->backends[i]);
    }

    furi_timer_free(app->anim_timer);
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);

    view_dispatcher_remove_view(app->view_dispatcher, RfidViewStatus);
    view_free(app->status);
    text_box_reset(app->text_box); // release the pointer into info_text first
    view_dispatcher_remove_view(app->view_dispatcher, RfidViewInfo);
    text_box_free(app->text_box);
    furi_string_free(app->info_text);
    view_dispatcher_remove_view(app->view_dispatcher, RfidViewMenu);
    submenu_free(app->menu);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t rfid_multi_reader_app(void* p) {
    UNUSED(p);
    RfidApp* app = rfid_app_alloc();
    rfid_switch_view(app, RfidViewMenu); // start on the band menu
    view_dispatcher_run(app->view_dispatcher);
    rfid_app_free(app);
    return 0;
}
