#include "reader_ui.h"
#include "../toolkit_ui.h"
#include <math.h>






// Small card silhouette with a chip, centred on (cx, cy).
static void draw_card_icon(Canvas* canvas, int cx, int cy) {
    canvas_draw_rframe(canvas, cx - 10, cy - 7, 21, 15, 2);
    canvas_draw_frame(canvas, cx - 7, cy - 4, 7, 6);
    canvas_draw_line(canvas, cx - 7, cy - 1, cx - 1, cy - 1);
    canvas_draw_line(canvas, cx - 4, cy - 4, cx - 4, cy + 1);
    canvas_draw_line(canvas, cx + 1, cy + 4, cx + 8, cy + 4);
}

// Radar: three arcs on each side of the card, expanding outwards and fading
// out (the outermost one is drawn sparsely) as `frame` advances.
static void draw_radar(Canvas* canvas, int cx, int cy, uint8_t frame) {
    for(int i = 0; i < 3; i++) {
        int r = 14 + ((frame + i * 4) % 12);
        // The widest ring is thinned out so the pulse reads as a fade.
        int step = (r > 22) ? 8 : 4;
        for(int a = -38; a <= 38; a += step) {
            float rad = (float)a * (float)M_PI / 180.0f;
            int dx = (int)(cosf(rad) * (float)r);
            int dy = (int)(sinf(rad) * (float)r);
            canvas_draw_dot(canvas, cx + dx, cy + dy);
            canvas_draw_dot(canvas, cx - dx, cy + dy);
        }
    }
}

static void draw_dots(Canvas* canvas, int x, int y, uint8_t frame) {
    int n = (frame / 4) % 4; // 0..3 dots, ~320 ms per step
    for(int i = 0; i < n; i++) {
        canvas_draw_dot(canvas, x + i * 4, y);
        canvas_draw_dot(canvas, x + i * 4 + 1, y);
    }
}

// Highlight frame around the active band / emulation label.
static void draw_band(Canvas* canvas) {
    canvas_draw_rframe(canvas, 10, 44, 108, 12, 3);
}

static void draw_state_scanning(Canvas* canvas, const ReaderModel* m) {
    const int cx = 64, cy = 30;
    draw_radar(canvas, cx, cy, m->frame);
    draw_card_icon(canvas, cx, cy);

    const char* active = m->lf ? "< 125 kHz RFID >" : "< 13.56 MHz NFC >";
    const char* idle = m->lf ? "13.56 MHz NFC" : "125 kHz RFID";

    // Active band boxed, the idle one left plain underneath.
    draw_band(canvas);
    ui_draw_centered(canvas, 53, active);

    int idle_w = canvas_string_width(canvas, idle);
    canvas_draw_str(canvas, 2, 63, idle);
    draw_dots(canvas, idle_w + 5, 62, m->frame);
    const char* hint = "OK:load";
    canvas_draw_str(canvas, UI_W - canvas_string_width(canvas, hint) - 2, 63, hint);
}

static void draw_state_reading(Canvas* canvas, const ReaderModel* m) {
    draw_card_icon(canvas, 64, 28);
    ui_draw_centered(canvas, 48, "Reading card");

    // Progress bar with a block sweeping left to right.
    canvas_draw_rframe(canvas, 14, 53, 100, 8, 2);
    int pos = (m->frame * 3) % 116; // 0..115, wraps past the right edge
    int x = 16 + pos - 24;
    int w = 24;
    if(x < 16) {
        w -= (16 - x);
        x = 16;
    }
    if(x + w > 112) w = 112 - x;
    if(w > 0) canvas_draw_box(canvas, x, 55, w, 4);
}

void reader_draw_callback(Canvas* canvas, void* model) {
    ReaderModel* m = model;
    canvas_clear(canvas);
    ui_status_bar(canvas, "UNIVERSAL READER", NULL, 0, 0, 0);

    switch(m->state) {
    case ReaderStateScanning:
        draw_state_scanning(canvas, m);
        break;

    case ReaderStateReading:
        draw_state_reading(canvas, m);
        break;

    case ReaderStateNotice:
        ui_status_bar(canvas, m->notice_title, NULL, 0, 0, 0);
        ui_notice(canvas, NULL, m->notice_l1, m->notice_l2);
        break;

    case ReaderStateEmulating:
        draw_radar(canvas, 64, 30, m->frame);
        draw_card_icon(canvas, 64, 30);
        draw_band(canvas);
        ui_draw_centered_fit(canvas, 53, m->emu_label, 100);
        ui_draw_centered(canvas, 63, "Back: stop");
        break;
    }
}

void reader_set_scanning(ReaderApp* app, bool lf) {
    with_view_model(
        app->view,
        ReaderModel * m,
        {
            m->state = ReaderStateScanning;
            m->lf = lf;
        },
        true);
}

// GUI thread only. Puts the status view (ReaderViewScan) into the emulating
// sub-state; reader_switch_view() starts anim_timer so the radar animates.
void reader_enter_emulating(ReaderApp* app, const char* type) {
    with_view_model(
        app->view,
        ReaderModel * m,
        {
            m->state = ReaderStateEmulating;
            snprintf(m->emu_label, sizeof(m->emu_label), "%s", type ? type : "card");
        },
        true);
    reader_switch_view(app, ReaderViewScan);
}

void reader_set_state(ReaderApp* app, ReaderState state) {
    with_view_model(app->view, ReaderModel * m, { m->state = state; }, true);
}

ReaderState reader_get_state(ReaderApp* app) {
    ReaderState state;
    with_view_model(app->view, ReaderModel * m, { state = m->state; }, false);
    return state;
}

void reader_bump_frame(ReaderApp* app) {
    with_view_model(app->view, ReaderModel * m, { m->frame++; }, true);
}

void reader_set_notice(ReaderApp* app, const char* title, const char* l1, const char* l2) {
    with_view_model(
        app->view,
        ReaderModel * m,
        {
            m->state = ReaderStateNotice;
            snprintf(m->notice_title, sizeof(m->notice_title), "%s", title);
            snprintf(m->notice_l1, sizeof(m->notice_l1), "%s", l1 ? l1 : "");
            snprintf(m->notice_l2, sizeof(m->notice_l2), "%s", l2 ? l2 : "");
        },
        true);
}

void reader_report_begin(ReaderApp* app) {
    text_box_reset(app->text_box); // drop the stale text pointer before rebuilding
    furi_string_reset(app->info_text);
}

void reader_report_show(ReaderApp* app, const char* header) {
    furi_string_cat_str(app->info_text, "\n[Back] = actions menu\n");
    submenu_set_header(app->actions, header);
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_focus(app->text_box, TextBoxFocusStart);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->info_text));
    reader_switch_view(app, ReaderViewInfo);
}
