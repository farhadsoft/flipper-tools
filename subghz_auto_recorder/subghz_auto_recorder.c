/*
 * SubGHz Auto Recorder for Flipper Zero
 * ------------------------------------------------------------
 * Listens on one user-picked frequency + modulation, auto-detects an
 * incoming transmission by RSSI threshold, and records it to its own RAW
 * .sub file under /ext/subghz/auto_rec/. Saved signals can be browsed,
 * replayed (TX), renamed or deleted.
 *
 * All subghz_devices_ / SubGhzWorker start/stop calls happen on the GUI
 * thread. Worker/receiver callbacks and the three timers only signal via
 * view_dispatcher_send_custom_event() or, for the rolling-code flag, a
 * single volatile field write.
 *
 * Legitimate use only: record and replay only devices you own or are
 * authorised to test.
 *
 * Target: Flipper Zero, Momentum fork verified this session (see CLAUDE.md).
 * Build with ufbt.
 */

#include <furi_hal.h>
#include <dialogs/dialogs.h>
#include <lib/subghz/blocks/generic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recorder_app.h"
#include "recorder_radio.h"
#include "recorder_ui.h"

/* ----------------------------- helpers ------------------------------ */

// Parallel labels for sub_rec_triggers[] (recorder_radio.h). VariableItem
// text is set from a string, and formatting a float would drag float printf
// into the binary for no reason. Kept here rather than in the shared header:
// recorder_radio.c never displays these, and a static array unused in a
// translation unit is a warning under -Wall.
static const char* const sub_rec_trigger_labels[] = {
    "-85 dBm",
    "-80 dBm",
    "-75 dBm",
    "-70 dBm",
    "-65 dBm",
    "-60 dBm",
};

void sub_rec_format_freq_line(SubRecApp* app, char* out, size_t out_size) {
    uint32_t f = app->custom_freq ? app->custom_freq : sub_rec_freqs[app->freq_idx];
    snprintf(
        out,
        out_size,
        "%lu.%02lu MHz  %s",
        (unsigned long)(f / 1000000),
        (unsigned long)(f / 10000 % 100),
        sub_rec_mods[app->mod_idx].label);
}

// storage_simply_mkdir() creates one level and returns true when the path
// already exists. The firmware only creates /ext/subghz itself, not our
// subfolder; without this, flipper_format_file_open_always() inside
// subghz_protocol_raw_save_to_file_init() fails.
static void sub_rec_ensure_dir(SubRecApp* app) {
    storage_simply_mkdir(app->storage, REC_DIR);
}

// Fail fast, before anything is allocated: a typo'd label would silently
// write Preset: FuriHalSubGhzPresetCustom into every capture --
// subghz_block_generic_get_preset_name() ends in an else yielding exactly
// that (lib/subghz/blocks/generic.c), so a mismatch here would only surface
// long after a capture, when this app's own Replay rejects the file.
static bool sub_rec_presets_self_check(void) {
    FuriString* tmp = furi_string_alloc();
    bool ok = true;
    for(size_t i = 0; i < COUNT_OF(sub_rec_mods) && ok; i++) {
        subghz_block_generic_get_preset_name(sub_rec_mods[i].label, tmp);
        if(furi_string_cmp_str(tmp, sub_rec_mods[i].file_preset) != 0) {
            FURI_LOG_E(
                TAG,
                "preset self-check: %s -> %s (want %s)",
                sub_rec_mods[i].label,
                furi_string_get_cstr(tmp),
                sub_rec_mods[i].file_preset);
            ok = false;
        }
    }
    furi_string_free(tmp);
    return ok;
}

void sub_rec_next_stem(SubRecApp* app, FuriString* out) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* base = furi_string_alloc_printf(
        "AR_%lu_%02u%02u%02u",
        (unsigned long)(app->preset.base.frequency / 100000), // 433920000 -> 4339
        dt.hour,
        dt.minute,
        dt.second);
    storage_get_next_filename(
        app->storage, REC_DIR, furi_string_get_cstr(base), ".sub", out, REC_STEM_MAX);
    furi_string_free(base);
}

/* --------------------------- view / notice --------------------------- */

// The only place that changes views. No animation timer exists in this app
// (see CLAUDE.md), so unlike universal_card_reader's reader_switch_view()
// there is nothing else to start or stop here.
void sub_rec_switch_view(SubRecApp* app, SubRecView view) {
    app->current_view = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

// GUI thread only. Shows a message as an overlay on the status view and
// auto-returns after NOTICE_MS, either chaining next_event or switching to
// return_view. Because a notice is an overlay, app->state is left untouched:
// a notice raised while listening leaves the radio armed and the RSSI tick
// running.
void sub_rec_show_notice(
    SubRecApp* app,
    const char* title,
    const char* l1,
    const char* l2,
    SubRecView return_view,
    SubRecCustomEvent next_event) {
    app->gen++;
    app->notice_active = true;
    app->notice_return_view = return_view;
    app->notice_next = next_event;
    sub_rec_set_notice(app, title, l1, l2, true);
    sub_rec_switch_view(app, SubRecViewStatus);
    furi_timer_start(app->notice_timer, furi_ms_to_ticks(NOTICE_MS));
}

/* ------------------------------- timers ------------------------------- */

// Runs on the TimersSrv thread -- posts only, never touches the radio or the
// view model.
static void sub_rec_rssi_timer_callback(void* context) {
    SubRecApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(SubRecEventRssiTick, app->gen));
}

static void sub_rec_tx_timer_callback(void* context) {
    SubRecApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(SubRecEventTxPoll, app->gen));
}

static void sub_rec_notice_timer_callback(void* context) {
    SubRecApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(SubRecEventNoticeDone, app->gen));
}

/* --------------------------- event handlers --------------------------- */

// Triggered from SubRecEventRssiTick on the GUI thread. Detection runs every
// tick; the repaint does not -- 40 Hz of with_view_model(..., true) would
// keep the GUI thread, which must also service capture open/close,
// permanently busy. Repaint on a state change, an above/below edge, or every
// RSSI_REDRAW_EVERY-th tick.
static void sub_rec_handle_rssi_tick(SubRecApp* app) {
    float rssi = subghz_devices_get_rssi(app->device);
    float trig = sub_rec_triggers[app->trigger_idx];
    bool above = (rssi >= trig);
    if(above) app->last_above_tick = furi_get_tick();

    app->tick_count++; // unconditional: see the force/redraw decimation below
    bool force = (above != app->last_above) || (app->state != app->last_state);
    bool redraw = force || ((app->tick_count % RSSI_REDRAW_EVERY) == 0);
    app->last_above = above;
    app->last_state = app->state;
    sub_rec_set_rssi(app, rssi, redraw && !app->notice_active);

    // A notice overlays the status view; never start a capture behind it.
    if(app->notice_active) return;

    if(app->state == SubRecStateArmed) {
        if(app->cooldown) {
            // The previous capture was ended by CAPTURE_MAX_MS while the
            // carrier was still up. Re-arm only on a real sub-threshold
            // sample -- through the setter, so app->cooldown and the
            // model's copy clear together.
            if(!above) sub_rec_set_state(app, SubRecStateArmed, false);
        } else if(above) {
            sub_rec_capture_begin(app);
        }
    } else if(app->state == SubRecStateRecording) {
        uint32_t now = furi_get_tick();
        bool hung = (now - app->last_above_tick) >= furi_ms_to_ticks(CAPTURE_HANG_MS);
        bool capped = (now - app->capture_start_tick) >= furi_ms_to_ticks(CAPTURE_MAX_MS);
        if(hung || capped) {
            sub_rec_capture_end(app, capped); // capped -> sets app->cooldown
        } else if(redraw) {
            sub_rec_set_samples(app, subghz_protocol_raw_get_sample_write(app->raw));
        }
    }
}

// Clears the notice overlay on both the app field and the model together --
// factored out so the two call sites (natural timeout, and Back dismissing
// it early) cannot drift out of sync with each other.
//
// The gen bump retires a SubRecEventNoticeDone the one-shot timer may have
// already posted: furi_timer_stop() cannot retract an event whose callback
// has run, so without it a Back pressed at ~NOTICE_MS is overtaken by that
// queued event, which then fires the chained action the user just cancelled
// -- for the rolling-code gate, an unwanted transmit. Clearing notice_next
// is the second half of the same cancel.
static void sub_rec_clear_notice(SubRecApp* app) {
    app->gen++;
    app->notice_active = false;
    app->notice_next = 0;
    sub_rec_set_notice(app, "", "", "", false);
}

static void sub_rec_handle_notice_done(SubRecApp* app) {
    SubRecCustomEvent next = app->notice_next;        // captured: clear resets it
    SubRecView return_view = app->notice_return_view; // captured for the same reason
    sub_rec_clear_notice(app);
    if(next != 0) {
        // Posted after the clear, so it carries the bumped gen.
        view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(next, app->gen));
    } else {
        sub_rec_switch_view(app, return_view);
    }
}

// First time "Listen" is chosen in a session, show the ethics gate; the
// chained event re-enters this handler, which now sees ethics_shown == true
// and starts listening. Subsequent Listen presses go straight to Armed.
static void sub_rec_handle_menu_listen(SubRecApp* app) {
    if(!app->ethics_shown) {
        app->ethics_shown = true;
        sub_rec_show_notice(
            app,
            "Own devices only",
            "record + replay only",
            "what you may test",
            SubRecViewMenu,
            SubRecEventMenuListen);
        return;
    }
    sub_rec_switch_view(app, SubRecViewStatus);
    sub_rec_listen_start(app);
}

// GUI thread only. dialog_file_browser_show() blocks this thread until the
// user picks or cancels, so every periodic poster must be stopped first
// (CLAUDE.md invariant 6): an 80 ms-class tick left running fills the
// dispatcher's 16-deep queue and blocks the TimersSrv thread for the whole
// dialog. Nothing here is periodic that fast, but the same rule applies to
// rssi_timer's 25 ms tick even more directly.
static void sub_rec_do_browse(SubRecApp* app) {
    furi_timer_stop(app->rssi_timer);
    furi_timer_stop(app->tx_timer);
    furi_timer_stop(app->notice_timer);
    sub_rec_ensure_dir(app);

    FuriString* path = furi_string_alloc_set_str(REC_DIR);
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, ".sub", NULL); // initialises every field
    opts.base_path = REC_DIR;

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    bool picked = dialog_file_browser_show(dialogs, path, path, &opts);
    furi_record_close(RECORD_DIALOGS);

    if(!picked) {
        furi_string_free(path);
        sub_rec_switch_view(app, SubRecViewMenu);
        return;
    }

    furi_string_set(app->selected_path, path);
    furi_string_free(path);
    app->rc_warned = false; // cleared whenever a file is (re)picked

    const char* full = furi_string_get_cstr(app->selected_path);
    const char* base = strrchr(full, '/');
    submenu_set_header(app->file_menu, base ? base + 1 : full);
    sub_rec_switch_view(app, SubRecViewFileMenu);
}

static void sub_rec_do_delete(SubRecApp* app) {
    const char* path = furi_string_get_cstr(app->selected_path);
    const char* base = strrchr(path, '/');
    char name[REC_TEXT_LINE_MAX];
    snprintf(name, sizeof(name), "%s", base ? base + 1 : path);

    // false means a real failure -- storage_simply_remove() also returns true
    // when the item is already gone (storage.h). Reporting "Deleted" on a
    // failed remove is a lie the user acts on.
    if(!storage_simply_remove(app->storage, path)) {
        FURI_LOG_E(TAG, "delete failed: %s", path);
        sub_rec_show_notice(app, "Delete failed", name, "", SubRecViewFileMenu, 0);
        return;
    }
    // Clear the selection and the warning flag so nothing can act on a file
    // that no longer exists.
    furi_string_reset(app->selected_path);
    app->rc_warned = false;

    sub_rec_show_notice(app, "Deleted", name, "", SubRecViewMenu, 0);
}

static void sub_rec_rename_result(void* context) {
    SubRecApp* app = context;

    size_t len = strlen(app->rename_buf);
    if(len == 0) {
        sub_rec_show_notice(app, "Rename failed", "empty name", "", SubRecViewMenu, 0);
        return;
    }

    const char* old_path = furi_string_get_cstr(app->selected_path);
    const char* old_base = strrchr(old_path, '/');
    old_base = old_base ? old_base + 1 : old_path;
    bool was_rc = strstr(old_base, "_RC") != NULL;
    // If the old basename ended in _RC, make sure the new one does too --
    // append it only when the user's name does not already end in _RC, so a
    // rename cannot produce ..._RC_RC. This is the only persistence of the
    // rolling-code warning; a free-text rename would otherwise silently
    // drop it.
    bool still_rc = (len >= 3) && (strcmp(app->rename_buf + len - 3, "_RC") == 0);

    char new_path[REC_PATH_MAX];
    if(was_rc && !still_rc) {
        snprintf(new_path, sizeof(new_path), "%s/%s_RC.sub", REC_DIR, app->rename_buf);
    } else {
        snprintf(new_path, sizeof(new_path), "%s/%s.sub", REC_DIR, app->rename_buf);
    }

    FS_Error err = storage_common_rename(app->storage, old_path, new_path);
    if(err != FSE_OK) {
        sub_rec_show_notice(
            app, "Rename failed", storage_error_get_desc(err), "", SubRecViewMenu, 0);
        return;
    }

    furi_string_set_str(app->selected_path, new_path);
    const char* base = strrchr(new_path, '/');
    sub_rec_show_notice(app, "Renamed", base ? base + 1 : new_path, "", SubRecViewMenu, 0);
}

static void sub_rec_do_rename_start(SubRecApp* app) {
    const char* path = furi_string_get_cstr(app->selected_path);
    const char* base = strrchr(path, '/');
    const char* name = base ? base + 1 : path;

    snprintf(app->rename_buf, sizeof(app->rename_buf), "%s", name);
    size_t len = strlen(app->rename_buf);
    if(len > 4 && strcmp(app->rename_buf + len - 4, ".sub") == 0) {
        app->rename_buf[len - 4] = '\0'; // edit the stem only, not the extension
    }

    text_input_set_header_text(app->text, "New name");
    text_input_set_result_callback(
        app->text, sub_rec_rename_result, app, app->rename_buf, sizeof(app->rename_buf), false);
    sub_rec_switch_view(app, SubRecViewText);
}

/* ------------------------------- settings ------------------------------ */

static void sub_rec_freq_changed(VariableItem* item) {
    SubRecApp* app = variable_item_get_context(item);
    size_t idx = variable_item_get_current_value_index(item);
    char buf[16];

    if(idx < COUNT_OF(sub_rec_freqs)) {
        app->freq_idx = (uint8_t)idx;
        app->custom_freq = 0;
        uint32_t f = sub_rec_freqs[idx];
        snprintf(
            buf,
            sizeof(buf),
            "%lu.%02lu MHz",
            (unsigned long)(f / 1000000),
            (unsigned long)(f / 10000 % 100));
    } else if(app->custom_freq) {
        // Re-entering the Custom slot with a value already set (e.g. after
        // scrolling away and back) -- keep showing it.
        snprintf(
            buf,
            sizeof(buf),
            "%lu.%02lu MHz",
            (unsigned long)(app->custom_freq / 1000000),
            (unsigned long)(app->custom_freq / 10000 % 100));
    } else {
        snprintf(buf, sizeof(buf), "Custom");
    }
    variable_item_set_current_value_text(item, buf);
}

static void sub_rec_freq_number_result(void* context, int32_t number) {
    SubRecApp* app = context;
    uint32_t freq = (uint32_t)number * 1000;

    if(!subghz_devices_is_frequency_valid(app->device, freq)) {
        sub_rec_show_notice(app, "Not a valid band", "", "", SubRecViewSettings, 0);
        return;
    }

    app->custom_freq = freq;
    variable_item_set_current_value_index(app->freq_item, (uint8_t)COUNT_OF(sub_rec_freqs));
    char buf[16];
    snprintf(
        buf,
        sizeof(buf),
        "%lu.%02lu MHz",
        (unsigned long)(freq / 1000000),
        (unsigned long)(freq / 10000 % 100));
    variable_item_set_current_value_text(app->freq_item, buf);
    sub_rec_switch_view(app, SubRecViewSettings);
}

static void sub_rec_mod_changed(VariableItem* item) {
    SubRecApp* app = variable_item_get_context(item);
    size_t idx = variable_item_get_current_value_index(item);
    app->mod_idx = (uint8_t)idx;
    variable_item_set_current_value_text(item, sub_rec_mods[idx].label);
}

static void sub_rec_trigger_changed(VariableItem* item) {
    SubRecApp* app = variable_item_get_context(item);
    size_t idx = variable_item_get_current_value_index(item);
    app->trigger_idx = (uint8_t)idx;
    variable_item_set_current_value_text(item, sub_rec_trigger_labels[idx]);

    // Move the RSSI bar's tick as soon as the setting changes, not only at
    // the next Listen. Recomputed rather than read back from the view model
    // -- recorder_ui.c is the only file allowed to do that.
    char line[24];
    sub_rec_format_freq_line(app, line, sizeof(line));
    sub_rec_set_freq_line(app, line, sub_rec_triggers[idx]);
}

// One callback for the whole list; index is the row's list position
// (0 = Frequency), not its value-index. Only the Frequency row, and only
// when its current value is the "Custom" slot, routes anywhere.
static void sub_rec_settings_enter_callback(void* context, uint32_t index) {
    SubRecApp* app = context;
    if(index != 0) return;
    if(variable_item_get_current_value_index(app->freq_item) != (uint8_t)COUNT_OF(sub_rec_freqs))
        return;

    uint32_t current = app->custom_freq ? app->custom_freq : sub_rec_freqs[app->freq_idx];
    number_input_set_header_text(app->number, "Frequency, kHz");
    number_input_set_result_callback(
        app->number, sub_rec_freq_number_result, app, (int32_t)(current / 1000), 300000, 928000);
    sub_rec_switch_view(app, SubRecViewNumber);
}

static void sub_rec_build_settings(SubRecApp* app) {
    VariableItem* item;
    char buf[16];

    item = variable_item_list_add(
        app->settings, "Frequency", (uint8_t)(COUNT_OF(sub_rec_freqs) + 1), sub_rec_freq_changed, app);
    variable_item_set_current_value_index(item, app->freq_idx);
    snprintf(
        buf,
        sizeof(buf),
        "%lu.%02lu MHz",
        (unsigned long)(sub_rec_freqs[app->freq_idx] / 1000000),
        (unsigned long)(sub_rec_freqs[app->freq_idx] / 10000 % 100));
    variable_item_set_current_value_text(item, buf);
    app->freq_item = item;

    item = variable_item_list_add(
        app->settings, "Modulation", (uint8_t)COUNT_OF(sub_rec_mods), sub_rec_mod_changed, app);
    variable_item_set_current_value_index(item, app->mod_idx);
    variable_item_set_current_value_text(item, sub_rec_mods[app->mod_idx].label);

    item = variable_item_list_add(
        app->settings, "Trigger", (uint8_t)COUNT_OF(sub_rec_triggers), sub_rec_trigger_changed, app);
    variable_item_set_current_value_index(item, app->trigger_idx);
    variable_item_set_current_value_text(item, sub_rec_trigger_labels[app->trigger_idx]);

    variable_item_list_set_enter_callback(app->settings, sub_rec_settings_enter_callback, app);
}

/* --------------------------- dispatcher wiring -------------------------- */

// Back that no view consumed. Runs on the GUI thread (input path), so
// stopping the radio and switching views here is legal -- mirrors
// universal_card_reader's reader_navigation_callback(); see that file for
// why this must be the dispatcher's navigation callback and never a
// per-view previous_callback.
static bool sub_rec_navigation_callback(void* context) {
    SubRecApp* app = context;

    if(app->current_view == SubRecViewStatus) {
        if(app->notice_active) {
            // A notice raised while listening returns to SubRecViewStatus
            // itself, so the first Back only dismisses the message; a
            // second Back then falls through to the state branch below.
            furi_timer_stop(app->notice_timer);
            sub_rec_clear_notice(app);
            sub_rec_switch_view(app, app->notice_return_view);
            return true;
        }
        switch(app->state) {
        case SubRecStateArmed:
        case SubRecStateRecording:
            sub_rec_listen_stop(app);
            sub_rec_switch_view(app, SubRecViewMenu);
            break;
        case SubRecStateSending:
            sub_rec_tx_stop(app);
            sub_rec_switch_view(app, SubRecViewFileMenu);
            break;
        case SubRecStateIdle:
        default:
            sub_rec_switch_view(app, SubRecViewMenu);
            break;
        }
        return true;
    }

    if(app->current_view == SubRecViewSettings || app->current_view == SubRecViewNumber ||
       app->current_view == SubRecViewFileMenu || app->current_view == SubRecViewText) {
        sub_rec_switch_view(app, SubRecViewMenu);
        return true;
    }

    // SubRecViewMenu: let the dispatcher stop -- run() returns and
    // sub_rec_app_free() does the rest.
    return false;
}

// Shared by both submenus: `index` is the SubRecCustomEvent the row was
// registered with, exactly like universal_card_reader's
// reader_action_callback().
static void sub_rec_menu_callback(void* context, uint32_t index) {
    SubRecApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(index, app->gen));
}

static bool sub_rec_custom_event_callback(void* context, uint32_t event) {
    SubRecApp* app = context;

    // Every timer in this app belongs to exactly one state, so a stale tick
    // must be dropped -- no event is exempt from this check.
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
    case SubRecEventRssiTick:
        sub_rec_handle_rssi_tick(app);
        return true;
    case SubRecEventTxPoll:
        sub_rec_handle_tx_poll(app);
        return true;
    case SubRecEventNoticeDone:
        sub_rec_handle_notice_done(app);
        return true;
    case SubRecEventMenuListen:
        sub_rec_handle_menu_listen(app);
        return true;
    case SubRecEventMenuSettings:
        sub_rec_switch_view(app, SubRecViewSettings);
        return true;
    case SubRecEventMenuSaved:
        sub_rec_do_browse(app);
        return true;
    case SubRecEventMenuExit:
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    case SubRecEventFileReplay:
        sub_rec_replay(app);
        return true;
    case SubRecEventFileDelete:
        sub_rec_do_delete(app);
        return true;
    case SubRecEventFileRename:
        sub_rec_do_rename_start(app);
        return true;
    case SubRecEventFileBack:
        sub_rec_switch_view(app, SubRecViewMenu);
        return true;
    default:
        return false;
    }
}

/* ------------------------------- app life ------------------------------ */

static SubRecApp* sub_rec_app_alloc(void) {
    // Fail fast, before anything is allocated: see sub_rec_presets_self_check().
    if(!sub_rec_presets_self_check()) {
        return NULL;
    }

    SubRecApp* app = malloc(sizeof(SubRecApp));
    memset(app, 0, sizeof(SubRecApp)); // zeroes app->preset too -- see recorder_app.h SubRecPreset

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->view = view_alloc();

    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(SubRecModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, sub_rec_draw_callback);
    // No input callback: the status view never consumes anything but Back,
    // which reaches sub_rec_navigation_callback() unconsumed by default.

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, sub_rec_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, sub_rec_navigation_callback);
    view_dispatcher_add_view(app->view_dispatcher, SubRecViewStatus, app->view);

    app->menu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, SubRecViewMenu, submenu_get_view(app->menu));
    submenu_add_item(app->menu, "Listen", SubRecEventMenuListen, sub_rec_menu_callback, app);
    submenu_add_item(app->menu, "Settings", SubRecEventMenuSettings, sub_rec_menu_callback, app);
    submenu_add_item(app->menu, "Saved signals", SubRecEventMenuSaved, sub_rec_menu_callback, app);
    submenu_add_item(app->menu, "Exit", SubRecEventMenuExit, sub_rec_menu_callback, app);

    app->file_menu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, SubRecViewFileMenu, submenu_get_view(app->file_menu));
    submenu_add_item(app->file_menu, "Replay", SubRecEventFileReplay, sub_rec_menu_callback, app);
    submenu_add_item(app->file_menu, "Rename", SubRecEventFileRename, sub_rec_menu_callback, app);
    submenu_add_item(app->file_menu, "Delete", SubRecEventFileDelete, sub_rec_menu_callback, app);
    submenu_add_item(app->file_menu, "Back", SubRecEventFileBack, sub_rec_menu_callback, app);

    app->settings = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, SubRecViewSettings, variable_item_list_get_view(app->settings));

    app->number = number_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, SubRecViewNumber, number_input_get_view(app->number));

    app->text = text_input_alloc();
    view_dispatcher_add_view(app->view_dispatcher, SubRecViewText, text_input_get_view(app->text));

    app->rssi_timer = furi_timer_alloc(sub_rec_rssi_timer_callback, FuriTimerTypePeriodic, app);
    app->tx_timer = furi_timer_alloc(sub_rec_tx_timer_callback, FuriTimerTypePeriodic, app);
    app->notice_timer = furi_timer_alloc(sub_rec_notice_timer_callback, FuriTimerTypeOnce, app);

    app->storage = furi_record_open(RECORD_STORAGE);
    sub_rec_ensure_dir(app);

    app->preset.base.name = furi_string_alloc();
    app->capture_path = furi_string_alloc();
    app->selected_path = furi_string_alloc();

    app->freq_idx = SUB_REC_FREQ_DEFAULT_IDX;
    app->mod_idx = 0;
    app->trigger_idx = SUB_REC_TRIGGER_DEFAULT_IDX;
    app->last_state = SubRecStateIdle;
    app->last_above = false;

    sub_rec_radio_alloc(app);
    sub_rec_build_settings(app);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    return app;
}

static void sub_rec_app_free(SubRecApp* app) {
    // Silence the posters first, then the radio, then the views -- mirrors
    // universal_card_reader's reader_app_free().
    furi_timer_stop(app->rssi_timer);
    furi_timer_stop(app->tx_timer);
    furi_timer_stop(app->notice_timer);

    sub_rec_listen_stop(app); // returns immediately when state == Idle (CLAUDE.md crash rule 5)
    if(app->state == SubRecStateSending) {
        sub_rec_tx_stop(app);
    } else {
        sub_rec_tx_abort(app);
    }
    sub_rec_radio_free(app);

    furi_timer_free(app->rssi_timer);
    furi_timer_free(app->tx_timer);
    furi_timer_free(app->notice_timer);

    view_dispatcher_remove_view(app->view_dispatcher, SubRecViewStatus);
    view_free(app->view);
    view_dispatcher_remove_view(app->view_dispatcher, SubRecViewMenu);
    submenu_free(app->menu);
    view_dispatcher_remove_view(app->view_dispatcher, SubRecViewFileMenu);
    submenu_free(app->file_menu);
    view_dispatcher_remove_view(app->view_dispatcher, SubRecViewSettings);
    variable_item_list_free(app->settings);
    view_dispatcher_remove_view(app->view_dispatcher, SubRecViewNumber);
    number_input_free(app->number);
    view_dispatcher_remove_view(app->view_dispatcher, SubRecViewText);
    text_input_free(app->text);

    furi_string_free(app->capture_path);
    furi_string_free(app->selected_path);
    furi_string_free(app->preset.base.name);

    furi_record_close(RECORD_STORAGE);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t subghz_auto_recorder_app(void* p) {
    UNUSED(p);
    SubRecApp* app = sub_rec_app_alloc();
    if(!app) return 0; // preset self-check failed; see sub_rec_presets_self_check()
    sub_rec_switch_view(app, SubRecViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    sub_rec_app_free(app);
    return 0;
}
