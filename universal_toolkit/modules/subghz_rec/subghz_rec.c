#include "subghz_rec.h"
#include "../../toolkit_log.h"
// recorder_app.h #define's its own TAG ("SubGhzAutoRec"); this file makes no
// FURI_LOG_* calls, so just drop toolkit_app.h's TAG rather than re-define it.
#undef TAG
#include "../../subghz_auto_recorder/recorder_app.h"

#include <furi_hal_rtc.h>
#include <stdio.h>

void subghz_rec_enter(ToolkitApp* app) {
    SubRecApp* ra = sub_rec_app_alloc(app->view_dispatcher);
    if(!ra) {
        // Preset self-check failed (sub_rec_presets_self_check() in
        // subghz_auto_recorder.c) -- no radio touched, nothing was
        // allocated. toolkit_enter_module() already set app->active to this
        // module before calling us, so app->active_ctx must not be left
        // NULL with app->active still set: the next dispatched event/nav
        // would reach subghz_rec_event()/subghz_rec_nav() with a NULL ctx
        // and crash. Log the failure and bounce straight back to the
        // launcher instead. toolkit_exit_module() calls this module's own
        // exit (subghz_rec_exit(), below) as part of that bounce -- its
        // NULL guard on app->active_ctx is what makes this safe.
        ToolkitLogRecord rec = {
            .ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysSubGhz};
        snprintf(rec.summary, sizeof(rec.summary), "subghz preset self-check failed");
        rec.file[0] = '\0';
        toolkit_log_append(app, &rec);
        toolkit_exit_module(app);
        return;
    }

    ra->toolkit = app;
    ra->module_mode = true;
    app->active_ctx = ra;
    sub_rec_switch_view(ra, SubRecViewMenu);

    ToolkitLogRecord rec = {.ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysSubGhz};
    snprintf(rec.summary, sizeof(rec.summary), "subghz recorder opened");
    rec.file[0] = '\0';
    toolkit_log_append(app, &rec);
}

void subghz_rec_exit(ToolkitApp* app) {
    SubRecApp* ra = app->active_ctx;
    // NULL when subghz_rec_enter() bounced back after a failed preset
    // self-check (toolkit_exit_module() -> m->exit() reaches here with
    // active_ctx never having been set) -- nothing to free in that case.
    if(!ra) return;
    sub_rec_app_free(ra);
    app->active_ctx = NULL;
}

bool subghz_rec_event(ToolkitApp* app, uint32_t id) {
    return sub_rec_custom_event_callback(app->active_ctx, id);
}

bool subghz_rec_nav(ToolkitApp* app) {
    return sub_rec_navigation_callback(app->active_ctx);
}
