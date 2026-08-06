#include "ble_findmy.h"
#include "findmy_payload.h"
#include "../../toolkit_log.h"
#include "../../toolkit_ui.h"

#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_rtc.h>
#include <extra_beacon.h>
#include <gui/view.h>

#define BLE_FINDMY_REFRESH_EVENT 0u
#define BLE_FINDMY_REFRESH_MS    500
// Apple Find My network advertisement interval: ~2 seconds.
#define BLE_FINDMY_ADV_INTERVAL_MS 2000

#undef TAG
#define TAG "BleFindMy"

typedef struct {
    View* view;
    FuriTimer* refresh_timer;
    bool beacon_active;
} BleFindMyCtx;

/* --------------------------------- draw ------------------------------------ */

static void ble_findmy_draw_callback(Canvas* canvas, void* model) {
    BleFindMyCtx* ctx = model;
    canvas_clear(canvas);
    toolkit_ui_draw_title_bar(canvas, "BLE Find My");

    char line[40];

    // Show the public key as two hex blocks to fit the 128 px screen.
    snprintf(line, sizeof(line), "Key: %02x%02x%02x%02x%02x%02x%02x%02x",
        findmy_public_key[0], findmy_public_key[1], findmy_public_key[2], findmy_public_key[3],
        findmy_public_key[4], findmy_public_key[5], findmy_public_key[6], findmy_public_key[7]);
    canvas_draw_str(canvas, 4, 24, line);

    snprintf(line, sizeof(line), "     %02x%02x%02x%02x%02x%02x%02x%02x",
        findmy_public_key[8], findmy_public_key[9], findmy_public_key[10], findmy_public_key[11],
        findmy_public_key[12], findmy_public_key[13], findmy_public_key[14], findmy_public_key[15]);
    canvas_draw_str(canvas, 4, 35, line);

    snprintf(line, sizeof(line), "     %02x%02x%02x%02x%02x%02x%02x%02x",
        findmy_public_key[16], findmy_public_key[17], findmy_public_key[18], findmy_public_key[19],
        findmy_public_key[20], findmy_public_key[21], findmy_public_key[22], findmy_public_key[23]);
    canvas_draw_str(canvas, 4, 46, line);

    snprintf(line, sizeof(line), "Beacon: %s", ctx->beacon_active ? "ON" : "OFF");
    canvas_draw_str(canvas, 4, 57, line);

    canvas_draw_str(canvas, 4, 63, "Back: exit module");
}

/* -------------------------------- timer ------------------------------------ */

static void ble_findmy_timer_callback(void* context) {
    ToolkitApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(BLE_FINDMY_REFRESH_EVENT, app->gen));
}

/* ------------------------------- lifecycle ---------------------------------- */

void ble_findmy_enter(ToolkitApp* app) {
    BleFindMyCtx* ctx = malloc(sizeof(BleFindMyCtx));
    ctx->beacon_active = furi_hal_bt_extra_beacon_is_active();
    app->active_ctx = ctx;

    ctx->view = view_alloc();
    view_set_draw_callback(ctx->view, ble_findmy_draw_callback);
    view_allocate_model(ctx->view, ViewModelTypeLocking, sizeof(BleFindMyCtx));
    view_dispatcher_add_view(app->view_dispatcher, TOOLKIT_VIEW_BASE_BLE_FINDMY, ctx->view);

    // Core2 must be running the BLE stack for the extra beacon APIs to work.
    if(!furi_hal_bt_ensure_c2_mode(BleGlueC2ModeStack)) {
        FURI_LOG_W(TAG, "failed to ensure C2 stack mode");
    }

    // Configure and start the extra beacon only if it is not already
    // running. The exit callback deliberately leaves the beacon active
    // (persistence is the feature), so re-entering the module must not
    // call start() on an already-running beacon — that triggers a
    // furi_check in the firmware.
    if(!furi_hal_bt_extra_beacon_is_active()) {
        GapExtraBeaconConfig config = {
            .min_adv_interval_ms = BLE_FINDMY_ADV_INTERVAL_MS,
            .max_adv_interval_ms = BLE_FINDMY_ADV_INTERVAL_MS,
            .adv_channel_map = GapAdvChannelMapAll,
            .adv_power_level = GapAdvPowerLevel_0dBm,
            .address_type = GapAddressTypeRandom,
            .address = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06},
        };
        if(!furi_hal_bt_extra_beacon_set_config(&config)) {
            FURI_LOG_W(TAG, "extra beacon set_config failed");
        }
        if(!furi_hal_bt_extra_beacon_set_data(findmy_public_key, FINDMY_KEY_LEN)) {
            FURI_LOG_W(TAG, "extra beacon set_data failed");
        }
        if(!furi_hal_bt_extra_beacon_start()) {
            FURI_LOG_W(TAG, "extra beacon start failed");
        }
    }

    ctx->refresh_timer = furi_timer_alloc(ble_findmy_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(ctx->refresh_timer, furi_ms_to_ticks(BLE_FINDMY_REFRESH_MS));

    view_dispatcher_switch_to_view(app->view_dispatcher, TOOLKIT_VIEW_BASE_BLE_FINDMY);

    ToolkitLogRecord rec = {.ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysBle};
    snprintf(rec.summary, sizeof(rec.summary), "ble findmy beacon started");
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

    view_dispatcher_remove_view(app->view_dispatcher, TOOLKIT_VIEW_BASE_BLE_FINDMY);
    view_free(ctx->view);
    free(ctx);
    app->active_ctx = NULL;
}

bool ble_findmy_event(ToolkitApp* app, uint32_t id) {
    if(id != BLE_FINDMY_REFRESH_EVENT) return false;
    BleFindMyCtx* ctx = app->active_ctx;

    ctx->beacon_active = furi_hal_bt_extra_beacon_is_active();

    with_view_model(ctx->view, BleFindMyCtx * m, { *m = *ctx; }, true);
    return true;
}

bool ble_findmy_nav(ToolkitApp* app) {
    toolkit_exit_module(app);
    return true;
}
