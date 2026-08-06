#include "toolkit_app.h"
#include "toolkit_log.h"
#include "modules/gpio_info.h"
#include "modules/card_reader.h"
#include "modules/rfid_multi.h"
#include "modules/subghz_rec.h"
#include "modules/ble_findmy.h"

// Append-only module table: each row assembles one module's descriptor from
// its exported callbacks. Phase 1 appends the three existing apps here, each
// wrapped as its own module -- against this same contract.
static const ToolkitModule modules[] = {
    {
        .name = "GPIO Info",
        .view_base = TOOLKIT_VIEW_BASE_GPIO,
        .enter = gpio_info_enter,
        .exit = gpio_info_exit,
        .event = gpio_info_event,
        .nav = gpio_info_nav,
    },
    {
        .name = "Card Reader",
        .view_base = TOOLKIT_VIEW_BASE_CARD_READER,
        .enter = card_reader_enter,
        .exit = card_reader_exit,
        .event = card_reader_event,
        .nav = card_reader_nav,
    },
    {
        .name = "RFID Multi",
        .view_base = TOOLKIT_VIEW_BASE_RFID_MULTI,
        .enter = rfid_multi_enter,
        .exit = rfid_multi_exit,
        .event = rfid_multi_event,
        .nav = rfid_multi_nav,
    },
    {
        .name = "SubGHz Recorder",
        .view_base = TOOLKIT_VIEW_BASE_SUBGHZ_REC,
        .enter = subghz_rec_enter,
        .exit = subghz_rec_exit,
        .event = subghz_rec_event,
        .nav = subghz_rec_nav,
    },
    {
        .name = "BLE Find My",
        .view_base = TOOLKIT_VIEW_BASE_BLE_FINDMY,
        .enter = ble_findmy_enter,
        .exit = ble_findmy_exit,
        .event = ble_findmy_event,
        .nav = ble_findmy_nav,
    },
};
#define TOOLKIT_MODULE_COUNT (sizeof(modules) / sizeof(modules[0]))

/* ---------------------------- module lifecycle ---------------------------- */

static void toolkit_show_launcher(ToolkitApp* app) {
    view_dispatcher_switch_to_view(app->view_dispatcher, TOOLKIT_VIEW_LAUNCHER);
}

static void toolkit_enter_module(ToolkitApp* app, const ToolkitModule* m) {
    app->active = m;
    m->enter(app); // registers views, allocs ctx, acquires peripheral, switches to its root view
}

void toolkit_exit_module(ToolkitApp* app) {
    const ToolkitModule* m = app->active;
    // Switch to the launcher BEFORE tearing the module down. ViewDispatcher's
    // remove_view() sets current_view to NULL -- and view_dispatcher_set_current_view()
    // unconditionally calls view_dispatcher_stop() when the new view is NULL --
    // whenever the view being removed is the one currently shown. Confirmed
    // against view_dispatcher.c: removing the module's view before switching
    // away from it stops the whole app, not just the module.
    toolkit_show_launcher(app);
    m->exit(app); // now safe: releases peripheral, frees ctx, removes its views
    app->gen++; // drop any queued event/timer tick of the module just torn down
    app->active = NULL;
}

/* -------------------------------- dispatch --------------------------------- */

static bool toolkit_launcher_event(ToolkitApp* app, uint32_t id) {
    if(id >= TOOLKIT_MODULE_COUNT) return false;
    toolkit_enter_module(app, &modules[id]);
    return true;
}

// Gen-filter first, then route by context: the active module's `event`, or
// the launcher's row selection when nothing is active.
static bool toolkit_custom_event(void* context, uint32_t packed) {
    ToolkitApp* app = context;
    if(EVENT_GEN(packed) != app->gen) return true; // drop stale (post-exit ticks, queued rows)
    uint32_t id = EVENT_ID(packed);
    return app->active ? app->active->event(app, id) : toolkit_launcher_event(app, id);
}

// The active module always owns Back and always returns true (handled); at
// the launcher root there is no active module, so returning false is what
// lets ViewDispatcher's navigation callback stop the app. The router always
// has a defined answer, so no unhandled view can silently exit mid-tree.
static bool toolkit_nav(void* context) {
    ToolkitApp* app = context;
    if(app->active) return app->active->nav(app);
    return false;
}

/* -------------------------------- launcher --------------------------------- */

static void toolkit_launcher_row_callback(void* context, uint32_t index) {
    ToolkitApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(index, app->gen));
}

static void toolkit_build_launcher(ToolkitApp* app) {
    submenu_set_header(app->launcher, "Universal Toolkit");
    for(uint32_t i = 0; i < TOOLKIT_MODULE_COUNT; i++) {
        submenu_add_item(app->launcher, modules[i].name, i, toolkit_launcher_row_callback, app);
    }
}

/* ------------------------------- alloc / free ------------------------------ */

static ToolkitApp* toolkit_app_alloc(void) {
    ToolkitApp* app = malloc(sizeof(ToolkitApp));
    memset(app, 0, sizeof(ToolkitApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->view_dispatcher = view_dispatcher_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, toolkit_custom_event);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, toolkit_nav);

    app->launcher = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, TOOLKIT_VIEW_LAUNCHER, submenu_get_view(app->launcher));
    toolkit_build_launcher(app);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    toolkit_show_launcher(app);

    return app;
}

static void toolkit_app_free(ToolkitApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, TOOLKIT_VIEW_LAUNCHER);
    submenu_free(app->launcher);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t toolkit_app(void* p) {
    UNUSED(p);
    ToolkitApp* app = toolkit_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    toolkit_app_free(app);
    return 0;
}
