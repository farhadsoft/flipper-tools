#include "rfid_multi.h"
#include "../../toolkit_log.h"
#undef TAG
#include "../../rfid_multi_reader/rfid_app.h"

#include <furi_hal_rtc.h>

void rfid_multi_enter(ToolkitApp* app) {
    RfidApp* ra = rfid_app_alloc(app->view_dispatcher);
    ra->toolkit = app;
    ra->module_mode = true;
    app->active_ctx = ra;
    rfid_switch_view(ra, RfidViewMenu);

    ToolkitLogRecord rec = {.ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysRfidLf};
    snprintf(rec.summary, sizeof(rec.summary), "rfid multi reader opened");
    rec.file[0] = '\0';
    toolkit_log_append(app, &rec);
}

void rfid_multi_exit(ToolkitApp* app) {
    RfidApp* ra = app->active_ctx;
    rfid_app_free(ra);
    app->active_ctx = NULL;
}

bool rfid_multi_event(ToolkitApp* app, uint32_t id) {
    return rfid_custom_event_callback(app->active_ctx, id);
}

bool rfid_multi_nav(ToolkitApp* app) {
    return rfid_navigation_callback(app->active_ctx);
}
