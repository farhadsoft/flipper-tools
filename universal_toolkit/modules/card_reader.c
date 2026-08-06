#include "card_reader.h"
#include "../toolkit_log.h"
#undef TAG
#include "../universal_card_reader/reader_app.h"
#include "../universal_card_reader/reader_nfc.h"

#include <furi_hal_rtc.h>

void card_reader_enter(ToolkitApp* app) {
    ReaderApp* ra = reader_app_alloc(app->view_dispatcher);
    ra->toolkit = app;
    ra->module_mode = true;
    app->active_ctx = ra;
    reader_start_nfc_phase(ra);

    ToolkitLogRecord rec = {.ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysNfc};
    snprintf(rec.summary, sizeof(rec.summary), "card reader opened");
    rec.file[0] = '\0';
    toolkit_log_append(app, &rec);
}

void card_reader_exit(ToolkitApp* app) {
    ReaderApp* ra = app->active_ctx;
    reader_app_free(ra);
    app->active_ctx = NULL;
}

bool card_reader_event(ToolkitApp* app, uint32_t id) {
    return reader_custom_event_callback(app->active_ctx, id);
}

bool card_reader_nav(ToolkitApp* app) {
    return reader_navigation_callback(app->active_ctx);
}
