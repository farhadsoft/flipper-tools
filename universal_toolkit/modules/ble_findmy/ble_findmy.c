#include "ble_findmy.h"
#include "findmy_payload.h"
#include "../../toolkit_log.h"
#include "../../toolkit_ui.h"

#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_power.h>
#include <furi_hal_rtc.h>
#include <extra_beacon.h>
#include <gui/view.h>
#include <gui/modules/submenu.h>
#include <input/input.h>
#include <storage/storage.h>
#include <string.h>

#define BLE_FINDMY_REFRESH_MS 500
// Apple Find My network advertisement interval: ~2 seconds.
#define BLE_FINDMY_ADV_INTERVAL_MS 2000

#define BLE_FINDMY_KEY_DIR  EXT_PATH("apps_data/ble_findmy")
#define BLE_FINDMY_MAX_KEYS 32
#define BLE_KEY_NAME_MAX    64 // longer names are skipped, not truncated
#define BLE_KEY_HINT_MAX    16 // "Key 00 01 02 03" + NUL

#undef TAG
#define TAG "BleFindMy"

typedef enum {
    BleFindMyViewKeyPick = 0, // native Submenu over key files
    BleFindMyViewStatus = 1, // custom draw (redesigned hero)
} BleFindMyView;

typedef enum { // module-local ids; key rows occupy 1..BLE_FINDMY_MAX_KEYS, all < 254
    BleFindMyEventRefresh = 0,
    BleFindMyEventKeyBase = 1,
    BleFindMyEventToggle = (BleFindMyEventKeyBase + BLE_FINDMY_MAX_KEYS), // = 33
} BleFindMyCustomEvent;

typedef struct {
    View* view; // status view (custom draw)
    Submenu* key_menu; // key-pick Submenu widget
    uint8_t active_key[FINDMY_PUBKEY_LEN]; // key the beacon was last (re)started with
    char key_names[BLE_FINDMY_MAX_KEYS][BLE_KEY_NAME_MAX]; // dir listing, built at enter
    uint8_t key_count;
    BleFindMyView current_view; // kept in sync by ble_findmy_switch_view()
    FuriTimer* refresh_timer;
    // Back-pointer: the Submenu item callback and the status view's input
    // callback both receive this ctx as their `context` (view_set_context()
    // below), not app, so they need this to reach view_dispatcher/gen. Same
    // pattern as ReaderApp.toolkit / SubRecApp.toolkit.
    ToolkitApp* app;
} BleFindMyCtx;

typedef struct {
    bool beacon_active;
    uint8_t phase; // wave pulse counter, bumped on each refresh tick
    uint8_t battery;
    char key_hint[BLE_KEY_HINT_MAX];
} BleFindMyModel;

/* --------------------------------- draw ------------------------------------ */

#define BLE_WAVE_X    16 // wave source center
#define BLE_WAVE_Y    32
#define BLE_WAVE_SRC_R 2
#define BLE_WAVE_ARC0  5 // on-air arc radii = base + phase
#define BLE_WAVE_ARC1  9
#define BLE_WAVE_ARC2 13

static void ble_findmy_draw_callback(Canvas* canvas, void* model) {
    BleFindMyModel* m = model;
    canvas_clear(canvas);
    ui_status_bar(canvas, "Find My", NULL, UiStatusBle, m->battery, m->phase);

    canvas_draw_disc(canvas, BLE_WAVE_X, BLE_WAVE_Y, BLE_WAVE_SRC_R);
    if(m->beacon_active) {
        uint8_t p = m->phase & 3;
        canvas_draw_circle(canvas, BLE_WAVE_X, BLE_WAVE_Y, BLE_WAVE_ARC0 + p);
        canvas_draw_circle(canvas, BLE_WAVE_X, BLE_WAVE_Y, BLE_WAVE_ARC1 + p);
        canvas_draw_circle(canvas, BLE_WAVE_X, BLE_WAVE_Y, BLE_WAVE_ARC2 + p);
    } else {
        canvas_draw_circle(canvas, BLE_WAVE_X, BLE_WAVE_Y, BLE_WAVE_ARC0);
        canvas_draw_circle(canvas, BLE_WAVE_X, BLE_WAVE_Y, BLE_WAVE_ARC1);
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 38, 24, m->beacon_active ? "On air" : "Stopped");
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_str(canvas, 38, 33, m->key_hint);

    char line[24];
    snprintf(
        line, sizeof(line), "Int %lu s", (unsigned long)(BLE_FINDMY_ADV_INTERVAL_MS / 1000));
    canvas_draw_str(canvas, 38, 42, line);

    canvas_draw_str(canvas, UI_MARGIN, UI_FOOTER_Y, m->beacon_active ? "OK: Stop" : "OK: Start");
}

/* -------------------------------- timer ------------------------------------ */

// Runs on the TimersSrv thread: posts only -- is_active()/battery read and
// with_view_model both happen in ble_findmy_event() on the GUI thread. Only
// running while the status view is showing (ble_findmy_switch_view()).
static void ble_findmy_timer_callback(void* context) {
    ToolkitApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(BleFindMyEventRefresh, app->gen));
}

/* -------------------------------- beacon ------------------------------------ */

static void ble_findmy_start_beacon(BleFindMyCtx* ctx) {
    uint8_t adv[FINDMY_ADV_LEN];
    findmy_build_adv(ctx->active_key, adv);

    GapExtraBeaconConfig config = {
        .min_adv_interval_ms = BLE_FINDMY_ADV_INTERVAL_MS,
        .max_adv_interval_ms = BLE_FINDMY_ADV_INTERVAL_MS,
        .adv_channel_map = GapAdvChannelMapAll,
        .adv_power_level = GapAdvPowerLevel_0dBm,
        .address_type = GapAddressTypeRandom,
    };
    // findmy_build_mac() returns the logical MSB-first MAC
    // (address[0] = pubkey[0] | 0xC0), matching the OpenHaystack reference.
    // STM32WB's aci_gap_additional_beacon_start sends the address bytes
    // LSB-first over the air, so reverse them here (not in the domain
    // function) so scanners observe the reference MAC order.
    findmy_build_mac(ctx->active_key, config.address);
    for(size_t i = 0; i < EXTRA_BEACON_MAC_ADDR_SIZE / 2; i++) {
        uint8_t tmp = config.address[i];
        config.address[i] = config.address[EXTRA_BEACON_MAC_ADDR_SIZE - 1 - i];
        config.address[EXTRA_BEACON_MAC_ADDR_SIZE - 1 - i] = tmp;
    }
    if(!furi_hal_bt_extra_beacon_set_config(&config)) {
        FURI_LOG_W(TAG, "extra beacon set_config failed");
    }
    if(!furi_hal_bt_extra_beacon_set_data(adv, sizeof(adv))) {
        FURI_LOG_W(TAG, "extra beacon set_data failed");
    }
    if(!furi_hal_bt_extra_beacon_start()) {
        FURI_LOG_W(TAG, "extra beacon start failed");
    }
}

static void ble_findmy_stop_beacon(BleFindMyCtx* ctx) {
    UNUSED(ctx);
    if(!furi_hal_bt_extra_beacon_stop()) {
        FURI_LOG_W(TAG, "extra beacon stop failed");
    }
}

/* --------------------------------- keys ------------------------------------- */

// Enumerates BLE_FINDMY_KEY_DIR into ctx->key_names/key_count. Skips
// directories, empty/too-long names, and anything past BLE_FINDMY_MAX_KEYS.
static void ble_findmy_enumerate_keys(BleFindMyCtx* ctx, Storage* storage) {
    ctx->key_count = 0;
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, BLE_FINDMY_KEY_DIR)) {
        FileInfo info;
        char name[BLE_KEY_NAME_MAX];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(ctx->key_count >= BLE_FINDMY_MAX_KEYS) break;
            if(info.flags & FSF_DIRECTORY) continue;
            size_t len = strlen(name);
            if(len == 0 || len >= BLE_KEY_NAME_MAX) continue;
            strcpy(ctx->key_names[ctx->key_count], name);
            ctx->key_count++;
        }
    }
    // storage_dir_open() docs (storage.h): storage_dir_close() must be
    // called even when the open failed -- never skip it inside the `if`.
    storage_dir_close(dir);
    storage_file_free(dir);
}

// mkdir ladder + enumerate; seeds one default key file (the compile-time
// example key) when the directory is empty, so a fresh install still has a
// pickable key and behaves like the old auto-start build after one pick.
static void ble_findmy_load_keys(BleFindMyCtx* ctx) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(storage, BLE_FINDMY_KEY_DIR);

    ble_findmy_enumerate_keys(ctx, storage);

    if(ctx->key_count == 0) {
        File* file = storage_file_alloc(storage);
        if(storage_file_open(
               file, BLE_FINDMY_KEY_DIR "/default.key", FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            char hex[FINDMY_PUBKEY_LEN * 2 + 1];
            for(size_t i = 0; i < FINDMY_PUBKEY_LEN; i++) {
                snprintf(&hex[i * 2], 3, "%02x", findmy_public_key[i]);
            }
            if(storage_file_write(file, hex, FINDMY_PUBKEY_LEN * 2) != FINDMY_PUBKEY_LEN * 2) {
                FURI_LOG_W(TAG, "seed key write short");
            }
        } else {
            FURI_LOG_W(TAG, "seed key open failed");
        }
        storage_file_close(file);
        storage_file_free(file);
        ble_findmy_enumerate_keys(ctx, storage);
    }

    furi_record_close(RECORD_STORAGE);
}

// Reads and hex-parses ctx->key_names[row] from disk into out. Returns false
// (out untouched) on any I/O or parse error -- callers keep whatever key was
// already active.
static bool ble_findmy_read_key(BleFindMyCtx* ctx, uint8_t row, uint8_t out[FINDMY_PUBKEY_LEN]) {
    char path[sizeof(BLE_FINDMY_KEY_DIR) + 1 + BLE_KEY_NAME_MAX];
    snprintf(path, sizeof(path), "%s/%s", BLE_FINDMY_KEY_DIR, ctx->key_names[row]);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[60];
        size_t n = storage_file_read(file, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        while(n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
            buf[--n] = '\0';
        }
        ok = findmy_parse_hex_key(buf, out);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* --------------------------------- views ------------------------------------ */

// The only place that changes views. Keeps current_view in sync (mirrors
// reader_switch_view()'s contract) and starts/stops the refresh timer: only
// the status view needs live is_active()/battery/phase updates, so a timer
// left running on the key submenu would post a tick into the dispatcher
// every BLE_FINDMY_REFRESH_MS for nothing.
static void ble_findmy_switch_view(BleFindMyCtx* ctx, BleFindMyView v) {
    ctx->current_view = v;
    if(v == BleFindMyViewStatus) {
        furi_timer_start(ctx->refresh_timer, furi_ms_to_ticks(BLE_FINDMY_REFRESH_MS));
    } else {
        furi_timer_stop(ctx->refresh_timer);
    }
    view_dispatcher_switch_to_view(ctx->app->view_dispatcher, TOOLKIT_VIEW_BASE_BLE_FINDMY + v);
}

/* ------------------------------- callbacks ----------------------------------- */

// Key submenu row selection. Runs on the GUI thread, but routes through the
// dispatcher anyway so key load + beacon start/stop stay inside
// ble_findmy_event() (project invariant -- verbatim reader_action_callback
// pattern).
static void ble_findmy_key_callback(void* context, uint32_t index) {
    BleFindMyCtx* ctx = context;
    view_dispatcher_send_custom_event(
        ctx->app->view_dispatcher, EVENT_MAKE(index, ctx->app->gen));
}

static bool ble_findmy_status_input_callback(InputEvent* event, void* context) {
    BleFindMyCtx* ctx = context;
    if(event->type != InputTypeShort) return false;

    if(event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(
            ctx->app->view_dispatcher, EVENT_MAKE(BleFindMyEventToggle, ctx->app->gen));
        return true;
    }
    if(event->key == InputKeyBack) {
        ble_findmy_switch_view(ctx, BleFindMyViewKeyPick);
        return true;
    }
    return false;
}

/* ------------------------------- lifecycle ---------------------------------- */

void ble_findmy_enter(ToolkitApp* app) {
    BleFindMyCtx* ctx = malloc(sizeof(BleFindMyCtx));
    app->active_ctx = ctx;
    ctx->app = app;

    ctx->view = view_alloc();
    view_allocate_model(ctx->view, ViewModelTypeLocking, sizeof(BleFindMyModel));
    view_set_draw_callback(ctx->view, ble_findmy_draw_callback);
    view_set_input_callback(ctx->view, ble_findmy_status_input_callback);
    view_set_context(ctx->view, ctx);
    view_dispatcher_add_view(
        app->view_dispatcher, TOOLKIT_VIEW_BASE_BLE_FINDMY + BleFindMyViewStatus, ctx->view);
    ctx->refresh_timer = furi_timer_alloc(ble_findmy_timer_callback, FuriTimerTypePeriodic, app);

    // Core2 must be running the BLE stack for the extra beacon APIs to work.
    if(!furi_hal_bt_ensure_c2_mode(BleGlueC2ModeStack)) {
        FURI_LOG_W(TAG, "failed to ensure C2 stack mode");
    }

    ble_findmy_load_keys(ctx);

    ctx->key_menu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        TOOLKIT_VIEW_BASE_BLE_FINDMY + BleFindMyViewKeyPick,
        submenu_get_view(ctx->key_menu));
    submenu_set_header(ctx->key_menu, "Find My keys");
    for(uint8_t r = 0; r < ctx->key_count; r++) {
        submenu_add_item(
            ctx->key_menu,
            ctx->key_names[r],
            BleFindMyEventKeyBase + r,
            ble_findmy_key_callback,
            ctx);
    }
    submenu_set_selected_item(ctx->key_menu, 0);

    // Beacon auto-start is gone: the beacon now only (re)starts when a key
    // is picked (ble_findmy_event()'s key-row branch). Land on the key
    // submenu; switch_view goes last, after both views exist -- routed
    // through the helper (not a direct assignment) so it stays the sole
    // writer of current_view.
    ble_findmy_switch_view(ctx, BleFindMyViewKeyPick);

    ToolkitLogRecord rec = {.ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysBle};
    snprintf(rec.summary, sizeof(rec.summary), "ble findmy opened");
    rec.file[0] = '\0';
    toolkit_log_append(app, &rec);
}

void ble_findmy_exit(ToolkitApp* app) {
    BleFindMyCtx* ctx = app->active_ctx;

    furi_timer_stop(ctx->refresh_timer);
    furi_timer_free(ctx->refresh_timer);

    // Lifecycle deviation: intentionally do NOT stop the extra beacon. The
    // beacon runs on Core2 independently of this FAP; stopping it here would
    // defeat the purpose of remaining discoverable by Apple's Find My network
    // after leaving the module. The user must power-cycle to disable it.

    view_dispatcher_remove_view(
        app->view_dispatcher, TOOLKIT_VIEW_BASE_BLE_FINDMY + BleFindMyViewStatus);
    view_free(ctx->view);
    view_dispatcher_remove_view(
        app->view_dispatcher, TOOLKIT_VIEW_BASE_BLE_FINDMY + BleFindMyViewKeyPick);
    submenu_free(ctx->key_menu);
    free(ctx);
    app->active_ctx = NULL;
}

bool ble_findmy_event(ToolkitApp* app, uint32_t id) {
    BleFindMyCtx* ctx = app->active_ctx;

    if(id == BleFindMyEventRefresh) {
        bool active = furi_hal_bt_extra_beacon_is_active();
        with_view_model(
            ctx->view,
            BleFindMyModel * m,
            {
                m->beacon_active = active;
                m->battery = furi_hal_power_get_pct();
                m->phase++;
            },
            true);
        return true;
    }

    if(id == BleFindMyEventToggle) {
        if(furi_hal_bt_extra_beacon_is_active()) {
            ble_findmy_stop_beacon(ctx);
        } else {
            ble_findmy_start_beacon(ctx);
        }
        bool now_active = furi_hal_bt_extra_beacon_is_active();
        with_view_model(ctx->view, BleFindMyModel * m, { m->beacon_active = now_active; }, true);

        ToolkitLogRecord rec = {.ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysBle};
        snprintf(
            rec.summary,
            sizeof(rec.summary),
            "%s",
            now_active ? "ble beacon started" : "ble beacon stopped");
        rec.file[0] = '\0';
        toolkit_log_append(app, &rec);
        return true;
    }

    if(id >= BleFindMyEventKeyBase && id < BleFindMyEventToggle) {
        uint8_t row = (uint8_t)(id - BleFindMyEventKeyBase);
        if(row >= ctx->key_count) return true; // stale/out-of-range row: ignore, stay put

        uint8_t parsed[FINDMY_PUBKEY_LEN];
        if(ble_findmy_read_key(ctx, row, parsed)) {
            memcpy(ctx->active_key, parsed, FINDMY_PUBKEY_LEN);
            // Double-start on a running beacon is a documented furi_check
            // hazard -- always stop first so picking a key is idempotent
            // even when one was already active.
            if(furi_hal_bt_extra_beacon_is_active()) ble_findmy_stop_beacon(ctx);
            ble_findmy_start_beacon(ctx);

            with_view_model(
                ctx->view,
                BleFindMyModel * m,
                {
                    m->beacon_active = true;
                    snprintf(
                        m->key_hint,
                        sizeof(m->key_hint),
                        "Key %02X %02X %02X %02X",
                        ctx->active_key[0],
                        ctx->active_key[1],
                        ctx->active_key[2],
                        ctx->active_key[3]);
                },
                true);

            ToolkitLogRecord rec = {.ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysBle};
            snprintf(rec.summary, sizeof(rec.summary), "ble beacon started");
            rec.file[0] = '\0';
            toolkit_log_append(app, &rec);

            ble_findmy_switch_view(ctx, BleFindMyViewStatus);
        } else {
            FURI_LOG_W(TAG, "bad key file %s", ctx->key_names[row]);
        }
        return true;
    }

    return false;
}

bool ble_findmy_nav(ToolkitApp* app) {
    toolkit_exit_module(app);
    return true;
}
