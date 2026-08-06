#include "brute_app.h"
#include "brute_worker.h"
#include "brute_ui.h"
#include "master_keys.h"
#include "crc8_dallas.h"

#include <storage/storage.h>
#include <lib/flipper_format/flipper_format.h>
#include <gui/elements.h>

#define BRUTE_NOTICE_MS 2500U
#define BRUTE_SEQ_TOTAL 100000U

static void brute_progress_save(BruteApp* app, bool force);
static void brute_number_input_callback(void* context, int32_t number);

static void brute_switch_view(BruteApp* app, BruteView view) {
    app->current_view = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

const char* brute_protocol_names[BruteProtocolCount] = {
    [BruteProtocolDallas] = BRUTE_PROTOCOL_DALLAS_GENERIC,
    [BruteProtocolCyfral] = BRUTE_PROTOCOL_CYFRAL,
    [BruteProtocolMetakom] = BRUTE_PROTOCOL_METAKOM,
};

const char* brute_protocol_item_name(BruteApp* app, BruteProtocolItem item) {
    UNUSED(app);
    if(item < BruteProtocolCount) return brute_protocol_names[item];
    return "?";
}

const char* brute_mode_name(BruteMode mode) {
    switch(mode) {
    case BruteModeMaster:
        return "Master";
    case BruteModeSequential:
        return "Sequential";
    default:
        return "?";
    }
}

/* Model setters: update both app state and the status view. */
void brute_set_state(BruteApp* app, BruteState state) {
    app->state = state;
    brute_status_set_state(app->status_view, state);
}

void brute_set_index(BruteApp* app, uint32_t index) {
    app->current_index = index;
    brute_status_set_index(app->status_view, index);
}

void brute_set_total(BruteApp* app, uint32_t total) {
    app->total_keys = total;
    brute_status_set_model(
        app->status_view,
        app->selected_mode,
        app->state,
        app->protocol_item,
        app->current_index,
        app->total_keys,
        app->model.key_data,
        app->model.key_data_len,
        app->model.keys_per_min,
        app->model.elapsed_ms,
        app->model.eta_ms,
        app->notice_active,
        app->ethics_accepted);
}

void brute_set_key(BruteApp* app, const uint8_t* data, size_t len) {
    app->model.key_data_len = len < sizeof(app->model.key_data) ? len : sizeof(app->model.key_data);
    memcpy(app->model.key_data, data, app->model.key_data_len);
    brute_status_set_key(app->status_view, data, len);
}

void brute_set_notice(BruteApp* app, bool active) {
    app->notice_active = active;
    brute_status_set_notice(app->status_view, active);
}

void brute_set_ethics(BruteApp* app, bool accepted) {
    app->ethics_accepted = accepted;
    brute_status_set_ethics(app->status_view, accepted);
}

void brute_set_protocol_item(BruteApp* app, BruteProtocolItem item) {
    app->protocol_item = item;
}

void brute_bump_keys_presented(BruteApp* app) {
    app->keys_presented++;
}

void brute_update_timing(BruteApp* app, uint32_t now_ms) {
    const uint32_t elapsed = now_ms - app->run_started_ms;
    uint32_t keys_per_min = 0;
    if(elapsed > 0 && app->keys_presented > 0) {
        keys_per_min = (uint32_t)((uint64_t)app->keys_presented * 60000ULL / (uint64_t)elapsed);
    }

    uint32_t eta_ms = 0;
    if(app->keys_presented > 0 && app->current_index < app->total_keys) {
        const uint32_t remaining = app->total_keys - app->current_index;
        eta_ms = (uint32_t)((uint64_t)remaining * elapsed / (uint64_t)app->keys_presented);
    }

    app->model.elapsed_ms = elapsed;
    app->model.keys_per_min = keys_per_min;
    app->model.eta_ms = eta_ms;
    brute_status_set_timing(app->status_view, keys_per_min, elapsed, eta_ms);
}

/* Menu callbacks. */
static void brute_menu_callback(void* context, uint32_t index) {
    BruteApp* app = context;
    uint32_t event = BruteEventMenuSettings;
    if(index == BruteMenuMaster) {
        event = BruteEventMenuMaster;
    } else if(index == BruteMenuSequential) {
        event = BruteEventMenuSequential;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(event, app->gen));
}

/* Settings: value change callbacks. */
static uint32_t brute_timing_ms_from_index(uint8_t index) {
    return BRUTE_TIMING_MIN_MS + (uint32_t)index * 50U;
}

static uint8_t brute_timing_index_from_ms(uint32_t ms) {
    if(ms < BRUTE_TIMING_MIN_MS) ms = BRUTE_TIMING_MIN_MS;
    if(ms > BRUTE_TIMING_MAX_MS) ms = BRUTE_TIMING_MAX_MS;
    return (uint8_t)((ms - BRUTE_TIMING_MIN_MS) / 50U);
}

static uint8_t brute_timing_index_count(void) {
    return (uint8_t)((BRUTE_TIMING_MAX_MS - BRUTE_TIMING_MIN_MS) / 50U) + 1U;
}

static void brute_settings_protocol_change(VariableItem* item) {
    BruteApp* app = variable_item_get_context(item);
    app->protocol_item = variable_item_get_current_value_index(item);
}

static void brute_settings_dwell_change(VariableItem* item) {
    BruteApp* app = variable_item_get_context(item);
    app->dwell_ms = brute_timing_ms_from_index(variable_item_get_current_value_index(item));
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)app->dwell_ms);
    variable_item_set_current_value_text(item, buf);
}

static void brute_settings_gap_change(VariableItem* item) {
    BruteApp* app = variable_item_get_context(item);
    app->gap_ms = brute_timing_ms_from_index(variable_item_get_current_value_index(item));
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)app->gap_ms);
    variable_item_set_current_value_text(item, buf);
}

static void brute_settings_resume_change(VariableItem* item) {
    BruteApp* app = variable_item_get_context(item);
    app->resume = variable_item_get_current_value_index(item) != 0;
    variable_item_set_current_value_text(item, app->resume ? "On" : "Off");
}

static void brute_settings_enter_callback(void* context, uint32_t index) {
    BruteApp* app = context;
    if(index == BruteSettingStartIndex) {
        /* Open number input for start index. */
        number_input_set_header_text(app->number_input, "Start index");
        number_input_set_result_callback(
            app->number_input,
            brute_number_input_callback,
            app,
            (int32_t)app->start_index,
            0,
            999999);
        brute_switch_view(app, BruteViewNumber);
    }
}

/* Number input callback. */
static void brute_number_input_callback(void* context, int32_t number) {
    BruteApp* app = context;
    app->start_index = (uint32_t)number;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(BruteEventNumberDone, app->gen));
}

static void brute_settings_setup(BruteApp* app) {
    app->settings = variable_item_list_alloc();
    variable_item_list_set_enter_callback(app->settings, brute_settings_enter_callback, app);

    VariableItem* item;
    char buf[16];

    item = variable_item_list_add(app->settings, "Protocol", BruteProtocolCount, brute_settings_protocol_change, app);
    variable_item_set_current_value_index(item, app->protocol_item);
    variable_item_set_current_value_text(item, brute_protocol_item_name(app, app->protocol_item));

    item = variable_item_list_add(app->settings, "Dwell", brute_timing_index_count(), brute_settings_dwell_change, app);
    variable_item_set_current_value_index(item, brute_timing_index_from_ms(app->dwell_ms));
    snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)app->dwell_ms);
    variable_item_set_current_value_text(item, buf);

    item = variable_item_list_add(app->settings, "Gap", brute_timing_index_count(), brute_settings_gap_change, app);
    variable_item_set_current_value_index(item, brute_timing_index_from_ms(app->gap_ms));
    snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)app->gap_ms);
    variable_item_set_current_value_text(item, buf);

    item = variable_item_list_add(app->settings, "Start index", 1, NULL, app);
    variable_item_set_current_value_index(item, 0);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)app->start_index);
    variable_item_set_current_value_text(item, buf);

    item = variable_item_list_add(app->settings, "Resume", 2, brute_settings_resume_change, app);
    variable_item_set_current_value_index(item, app->resume ? 1 : 0);
    variable_item_set_current_value_text(item, app->resume ? "On" : "Off");
}

/* Key generation / loading. */
static void brute_load_master_key(BruteApp* app, uint32_t index) {
    furi_check(index < master_keys_count);
    MasterKey* mk = &master_keys[index];
    brute_worker_emulate_key(app, mk->protocol_id, mk->data, sizeof(mk->data));
    brute_set_key(app, mk->data, sizeof(mk->data));
    FURI_LOG_D("Brute", "present master %lu: %s", (unsigned long)index, mk->name);
}

static void brute_generate_dallas_key(uint8_t family, uint32_t serial, uint8_t* out) {
    out[0] = family;
    out[1] = (uint8_t)(serial >> 0);
    out[2] = (uint8_t)(serial >> 8);
    out[3] = (uint8_t)(serial >> 16);
    out[4] = (uint8_t)(serial >> 24);
    out[5] = 0x00;
    out[6] = 0x00;
    out[7] = crc8_dallas(out, 7);
}

static void brute_generate_cyfral_key(uint32_t serial, uint8_t* out) {
    out[0] = (uint8_t)(serial >> 0);
    out[1] = (uint8_t)(serial >> 8);
}

static void brute_generate_metakom_key(uint32_t serial, uint8_t* out) {
    out[0] = (uint8_t)(serial >> 0);
    out[1] = (uint8_t)(serial >> 8);
    out[2] = (uint8_t)(serial >> 16);
    out[3] = (uint8_t)(serial >> 24);
}

static void brute_load_sequential_key(BruteApp* app, uint32_t index) {
    uint8_t data[8] = {0};
    size_t len = 0;
    const uint32_t serial = index; /* 32-bit serial portion; high bytes zeroed for Dallas. */

    switch(app->protocol_item) {
    case BruteProtocolDallas:
        brute_generate_dallas_key(app->family, serial, data);
        len = 8;
        break;
    case BruteProtocolCyfral:
        brute_generate_cyfral_key(serial, data);
        len = 2;
        break;
    case BruteProtocolMetakom:
        brute_generate_metakom_key(serial, data);
        len = 4;
        break;
    default:
        break;
    }

    iButtonProtocolId protocol_id = ibutton_protocols_get_id_by_name(
        app->protocols, brute_protocol_item_name(app, app->protocol_item));
    if(protocol_id != iButtonProtocolIdInvalid) {
        brute_worker_emulate_key(app, protocol_id, data, len);
    }
    brute_set_key(app, data, len);
    FURI_LOG_D("Brute", "present sequential %lu", (unsigned long)index);
}

static void brute_present_key(BruteApp* app, uint32_t index) {
    if(app->selected_mode == BruteModeMaster) {
        if(index < master_keys_count) {
            brute_load_master_key(app, index);
        }
    } else {
        brute_load_sequential_key(app, index);
    }
    brute_set_index(app, index);
    brute_bump_keys_presented(app);
}

/* Run control. */
static void brute_run_stop(BruteApp* app);

static void brute_run_start(BruteApp* app, BruteMode mode) {
    app->gen++;
    app->selected_mode = mode;
    app->state = BruteStateIdle;
    app->keys_presented = 0;
    app->run_started_ms = furi_get_tick();
    app->key_started_ms = app->run_started_ms;
    app->current_index = 0;
    app->total_keys = 0;
    app->notice_active = false;

    const uint32_t resume_index = app->resume ? app->model.index : 0;
    memset(&app->model, 0, sizeof(app->model));

    if(mode == BruteModeMaster) {
        app->total_keys = master_keys_count;
        app->current_index = 0;
        if(resume_index > 0 && resume_index < app->total_keys) {
            app->current_index = resume_index;
        }
    } else {
        app->total_keys = BRUTE_SEQ_TOTAL;
        app->current_index = app->start_index;
        if(resume_index > app->start_index && resume_index < app->total_keys) {
            app->current_index = resume_index;
        }
    }

    if(!brute_worker_start(app)) {
        FURI_LOG_E("Brute", "failed to start worker");
        return;
    }

    FURI_LOG_I(
        "Brute",
        "start %s at %lu/%lu, dwell=%lu gap=%lu",
        brute_mode_name(mode),
        (unsigned long)app->current_index,
        (unsigned long)app->total_keys,
        (unsigned long)app->dwell_ms,
        (unsigned long)app->gap_ms);

    brute_set_protocol_item(app, app->protocol_item);
    brute_set_total(app, app->total_keys);
    brute_set_index(app, app->current_index);
    brute_set_state(app, BruteStateArmed);
    brute_set_notice(app, !app->ethics_accepted);

    brute_switch_view(app, BruteViewStatus);
}

static void brute_run_stop(BruteApp* app) {
    if(app->state != BruteStateIdle) {
        brute_worker_emulate_stop(app);
    }
    brute_set_state(app, BruteStateIdle);
    brute_set_notice(app, false);
    brute_progress_save(app, true);

    brute_switch_view(app, BruteViewMenu);
    FURI_LOG_I("Brute", "stop at index %lu", (unsigned long)app->current_index);
}

static void brute_run_done(BruteApp* app) {
    brute_worker_emulate_stop(app);
    brute_set_state(app, BruteStateDone);
    brute_set_notice(app, false);
    brute_progress_save(app, true);
    FURI_LOG_I("Brute", "done at index %lu", (unsigned long)app->current_index);
}

/* Tick-driven state machine. */
static void brute_tick_handler(BruteApp* app) {
    const uint32_t now = furi_get_tick();

    if(app->state == BruteStateIdle) {
        return;
    }

    /* Update live timing every tick. */
    brute_update_timing(app, now);

    if(app->state == BruteStateArmed) {
        if(app->notice_active) {
            if((now - app->key_started_ms) >= BRUTE_NOTICE_MS) {
                brute_set_notice(app, false);
                app->ethics_accepted = true;
                brute_progress_save(app, false);
            } else {
                return;
            }
        }
        /* Start presenting the first key. */
        brute_present_key(app, app->current_index);
        app->key_started_ms = now;
        brute_set_state(app, BruteStatePresent);
        return;
    }

    if(app->state == BruteStatePresent) {
        if((now - app->key_started_ms) >= app->dwell_ms) {
            brute_worker_emulate_stop(app);
            app->key_started_ms = now;
            brute_set_state(app, BruteStateGap);
        }
        return;
    }

    if(app->state == BruteStateGap) {
        if((now - app->key_started_ms) >= app->gap_ms) {
            const uint32_t next = app->current_index + 1;
            if(next >= app->total_keys) {
                brute_run_done(app);
            } else {
                brute_present_key(app, next);
                app->key_started_ms = now;
                brute_set_state(app, BruteStatePresent);

                if(next % BRUTE_PROGRESS_SAVE_EVERY == 0) {
                    brute_progress_save(app, false);
                }
            }
        }
        return;
    }
}

/* Persistence. */
static void brute_progress_load(BruteApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool loaded = false;

    do {
        if(!flipper_format_file_open_existing(ff, BRUTE_PROGRESS_FILE)) break;
        FuriString* tmp = furi_string_alloc();
        uint32_t version = 0;
        if(!flipper_format_read_header(ff, tmp, &version)) {
            furi_string_free(tmp);
            break;
        }
        if(!furi_string_equal_str(tmp, "IBF Progress") || version != 1) {
            furi_string_free(tmp);
            break;
        }
        furi_string_free(tmp);

        uint32_t mode = 0;
        uint32_t protocol = 0;
        uint32_t index = 0;
        uint32_t total = 0;
        uint32_t dwell = BRUTE_DWELL_DEFAULT_MS;
        uint32_t gap = BRUTE_GAP_DEFAULT_MS;
        uint32_t family = BRUTE_SEQ_FAMILY_DEFAULT;
        uint32_t start_index = BRUTE_SEQ_INDEX_DEFAULT;
        uint32_t resume = 0;
        uint32_t ethics = 0;

        flipper_format_read_uint32(ff, "Mode", &mode, 1);
        flipper_format_read_uint32(ff, "Protocol", &protocol, 1);
        flipper_format_read_uint32(ff, "Index", &index, 1);
        flipper_format_read_uint32(ff, "Total", &total, 1);
        flipper_format_read_uint32(ff, "Dwell", &dwell, 1);
        flipper_format_read_uint32(ff, "Gap", &gap, 1);
        flipper_format_read_uint32(ff, "Family", &family, 1);
        flipper_format_read_uint32(ff, "StartIndex", &start_index, 1);
        flipper_format_read_uint32(ff, "Resume", &resume, 1);
        flipper_format_read_uint32(ff, "Ethics", &ethics, 1);

        app->selected_mode = (mode < BruteModeSequential + 1) ? mode : BruteModeMaster;
        app->protocol_item = (protocol < BruteProtocolCount) ? protocol : BruteProtocolDallas;
        app->model.index = index;
        app->total_keys = total;
        app->dwell_ms = dwell;
        app->gap_ms = gap;
        app->family = (uint8_t)family;
        app->start_index = start_index;
        app->resume = resume != 0;
        app->ethics_accepted = ethics != 0;
        loaded = true;
    } while(false);

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);

    if(!loaded) {
        app->selected_mode = BruteModeMaster;
        app->protocol_item = BruteProtocolDallas;
        app->dwell_ms = BRUTE_DWELL_DEFAULT_MS;
        app->gap_ms = BRUTE_GAP_DEFAULT_MS;
        app->start_index = BRUTE_SEQ_INDEX_DEFAULT;
        app->family = BRUTE_SEQ_FAMILY_DEFAULT;
        app->resume = false;
        app->ethics_accepted = false;
        app->model.index = 0;
        app->total_keys = 0;
    }

    FURI_LOG_I("Brute", "progress load: %s", loaded ? "ok" : "defaults");
}

static void brute_progress_save(BruteApp* app, bool force) {
    UNUSED(force);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_buffered_file_alloc(storage);
    bool ok = false;

    do {
        const uint32_t mode = app->selected_mode;
        const uint32_t protocol = app->protocol_item;
        const uint32_t index = app->current_index;
        const uint32_t total = app->total_keys;
        const uint32_t dwell = app->dwell_ms;
        const uint32_t gap = app->gap_ms;
        const uint32_t family = app->family;
        const uint32_t start_index = app->start_index;
        const uint32_t resume = app->resume ? 1U : 0U;
        const uint32_t ethics = app->ethics_accepted ? 1U : 0U;

        storage_common_mkdir(storage, BRUTE_APP_FOLDER);
        if(!flipper_format_buffered_file_open_always(ff, BRUTE_PROGRESS_FILE)) break;
        if(!flipper_format_write_header_cstr(ff, "IBF Progress", 1)) break;
        if(!flipper_format_write_uint32(ff, "Mode", &mode, 1)) break;
        if(!flipper_format_write_uint32(ff, "Protocol", &protocol, 1)) break;
        if(!flipper_format_write_uint32(ff, "Index", &index, 1)) break;
        if(!flipper_format_write_uint32(ff, "Total", &total, 1)) break;
        if(!flipper_format_write_uint32(ff, "Dwell", &dwell, 1)) break;
        if(!flipper_format_write_uint32(ff, "Gap", &gap, 1)) break;
        if(!flipper_format_write_uint32(ff, "Family", &family, 1)) break;
        if(!flipper_format_write_uint32(ff, "StartIndex", &start_index, 1)) break;
        if(!flipper_format_write_uint32(ff, "Resume", &resume, 1)) break;
        if(!flipper_format_write_uint32(ff, "Ethics", &ethics, 1)) break;
        ok = true;
    } while(false);

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);

    FURI_LOG_D("Brute", "progress save: %s", ok ? "ok" : "failed");
}

/* Navigation / event callbacks. */
static bool brute_navigation_event_callback(void* context) {
    BruteApp* app = context;

    if(app->state != BruteStateIdle) {
        brute_run_stop(app);
        return true;
    }

    if(app->current_view == BruteViewSettings) {
        brute_switch_view(app, BruteViewMenu);
        return true;
    }
    if(app->current_view == BruteViewNumber) {
        brute_switch_view(app, BruteViewSettings);
        return true;
    }

    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static bool brute_custom_event_callback(void* context, uint32_t event) {
    BruteApp* app = context;
    const uint32_t id = EVENT_ID(event);
    const uint32_t gen = EVENT_GEN(event);

    if(id == BruteEventTick) {
        brute_tick_handler(app);
        return true;
    }

    if(id == BruteEventEmulated) {
        FURI_LOG_D("Brute", "key emulated by reader");
        return true;
    }

    if(gen != app->gen) {
        FURI_LOG_D("Brute", "drop stale event %lu gen %lu", (unsigned long)id, (unsigned long)gen);
        return true;
    }

    switch(id) {
    case BruteEventMenuMaster:
        brute_run_start(app, BruteModeMaster);
        return true;
    case BruteEventMenuSequential:
        brute_run_start(app, BruteModeSequential);
        return true;
    case BruteEventMenuSettings:
        brute_switch_view(app, BruteViewSettings);
        return true;
    case BruteEventNumberDone:
        brute_switch_view(app, BruteViewSettings);
        return true;
    case BruteEventBack:
        return true;
    default:
        return true;
    }
}

static void brute_tick_callback(void* context) {
    BruteApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(BruteEventTick, app->gen));
}

static BruteApp* brute_app_alloc(void) {
    BruteApp* app = malloc(sizeof(BruteApp));
    memset(app, 0, sizeof(BruteApp));

    brute_progress_load(app);

    if(!brute_worker_setup(app)) {
        FURI_LOG_E("Brute", "worker setup failed");
        brute_worker_teardown(app);
        free(app);
        return NULL;
    }

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, brute_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, brute_navigation_event_callback);

    app->menu = submenu_alloc();
    submenu_add_item(app->menu, "Master keys", BruteMenuMaster, brute_menu_callback, app);
    submenu_add_item(app->menu, "Sequential walk", BruteMenuSequential, brute_menu_callback, app);
    submenu_add_item(app->menu, "Settings", BruteMenuSettings, brute_menu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, BruteViewMenu, submenu_get_view(app->menu));

    brute_settings_setup(app);
    view_dispatcher_add_view(
        app->view_dispatcher, BruteViewSettings, variable_item_list_get_view(app->settings));

    app->number_input = number_input_alloc();
    number_input_set_result_callback(
        app->number_input, brute_number_input_callback, app, 0, 0, 999999);
    view_dispatcher_add_view(
        app->view_dispatcher, BruteViewNumber, number_input_get_view(app->number_input));

    app->status_view = brute_status_view_alloc(app);
    view_dispatcher_add_view(app->view_dispatcher, BruteViewStatus, app->status_view);

    /* Initial full model. */
    app->model.mode = BruteModeMaster;
    app->model.state = BruteStateIdle;
    app->model.protocol_item = app->protocol_item;
    app->model.total = 0;
    app->model.ethics_accepted = app->ethics_accepted;
    brute_status_set_model(
        app->status_view,
        BruteModeMaster,
        BruteStateIdle,
        app->protocol_item,
        app->model.index,
        0,
        app->model.key_data,
        0,
        0,
        0,
        0,
        false,
        app->ethics_accepted);

    app->tick_timer = furi_timer_alloc(brute_tick_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->tick_timer, BRUTE_ANIM_PERIOD_MS);

    return app;
}

static void brute_app_free(BruteApp* app) {
    furi_timer_stop(app->tick_timer);
    furi_timer_free(app->tick_timer);

    if(app->state != BruteStateIdle) {
        brute_worker_emulate_stop(app);
        brute_set_state(app, BruteStateIdle);
        brute_set_notice(app, false);
        brute_progress_save(app, true);
    }

    view_dispatcher_remove_view(app->view_dispatcher, BruteViewStatus);
    brute_status_view_free(app->status_view);

    view_dispatcher_remove_view(app->view_dispatcher, BruteViewNumber);
    number_input_free(app->number_input);

    view_dispatcher_remove_view(app->view_dispatcher, BruteViewSettings);
    variable_item_list_free(app->settings);

    view_dispatcher_remove_view(app->view_dispatcher, BruteViewMenu);
    submenu_free(app->menu);

    view_dispatcher_free(app->view_dispatcher);

    brute_worker_teardown(app);
    free(app);
}

int32_t ibutton_bruteforce_app(void* p) {
    UNUSED(p);

    BruteApp* app = brute_app_alloc();
    if(!app) {
        return -1;
    }

    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    brute_switch_view(app, BruteViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    brute_app_free(app);
    return 0;
}
