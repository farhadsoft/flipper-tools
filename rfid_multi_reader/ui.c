#include "ui.h"
#include <math.h>

#define SCREEN_W    128
#define TITLE_BAR_H 13

static void draw_centered(Canvas* canvas, int cy, const char* str) {
    int w = canvas_string_width(canvas, str);
    canvas_draw_str(canvas, (SCREEN_W - w) / 2, cy, str);
}

// Like draw_centered(), but truncates with "..." until the string fits inside
// `max` pixels, so a long file name or protocol name never runs off-screen.
static void draw_centered_fit(Canvas* canvas, int cy, const char* str, int max) {
    char buf[56];
    snprintf(buf, sizeof(buf), "%s", str);
    size_t len = strlen(buf);
    while(len > 3 && canvas_string_width(canvas, buf) > max) {
        buf[--len] = '\0';
        buf[len - 1] = '.';
        buf[len - 2] = '.';
    }
    draw_centered(canvas, cy, buf);
}

// Full-width inverted title bar.
static void draw_title_bar(Canvas* canvas, const char* title) {
    canvas_draw_box(canvas, 0, 0, SCREEN_W, TITLE_BAR_H);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    int w = canvas_string_width(canvas, title);
    canvas_draw_str(canvas, (SCREEN_W - w) / 2, 10, title);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
}

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

// Highlight frame around the active band label.
static void draw_band(Canvas* canvas) {
    canvas_draw_rframe(canvas, 10, 44, 108, 12, 3);
}

static void draw_state_scanning(Canvas* canvas, const RfidModel* m) {
    const int cx = 64, cy = 30;
    draw_radar(canvas, cx, cy, m->frame);
    draw_card_icon(canvas, cx, cy);

    // Active band boxed; the mode name left plain underneath with
    // trailing animated dots, mirroring universal_card_reader's
    // active/idle two-line layout.
    draw_band(canvas);
    draw_centered(canvas, 53, m->band);

    int mode_w = canvas_string_width(canvas, m->mode);
    canvas_draw_str(canvas, (SCREEN_W - mode_w) / 2 - 8, 63, m->mode);
    draw_dots(canvas, (SCREEN_W + mode_w) / 2 - 2, 62, m->frame);
}

static void draw_state_reading(Canvas* canvas, const RfidModel* m) {
    draw_card_icon(canvas, 64, 28);
    draw_centered(canvas, 48, "Reading card");

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

void rfid_draw_callback(Canvas* canvas, void* model) {
    RfidModel* m = model;
    canvas_clear(canvas);
    draw_title_bar(canvas, "RFID MULTI-READER");

    switch(m->state) {
    case RfidStateScanning:
        draw_state_scanning(canvas, m);
        break;

    case RfidStateReading:
        draw_state_reading(canvas, m);
        break;

    case RfidStateNotice:
        canvas_set_font(canvas, FontPrimary);
        draw_centered(canvas, 28, m->notice_title);
        canvas_set_font(canvas, FontSecondary);
        draw_centered_fit(canvas, 42, m->notice_l1, 124);
        draw_centered_fit(canvas, 53, m->notice_l2, 124);
        break;

    default:
        // RfidStateIdle / RfidStateResult never reach this view (the menu
        // and the TextBox are separate views); nothing to draw for them.
        break;
    }
}

void rfid_set_state(RfidApp* app, RfidState state) {
    app->state = state;
    with_view_model(app->status, RfidModel * m, { m->state = state; }, true);
}

void rfid_set_scanning(RfidApp* app, const char* band, const char* mode) {
    with_view_model(
        app->status,
        RfidModel * m,
        {
            snprintf(m->band, sizeof(m->band), "%s", band);
            snprintf(m->mode, sizeof(m->mode), "%s", mode);
        },
        true);
}

void rfid_set_notice(RfidApp* app, const char* title, const char* l1, const char* l2) {
    with_view_model(
        app->status,
        RfidModel * m,
        {
            snprintf(m->notice_title, sizeof(m->notice_title), "%s", title);
            snprintf(m->notice_l1, sizeof(m->notice_l1), "%s", l1 ? l1 : "");
            snprintf(m->notice_l2, sizeof(m->notice_l2), "%s", l2 ? l2 : "");
        },
        true);
}

void rfid_bump_frame(RfidApp* app) {
    with_view_model(app->status, RfidModel * m, { m->frame++; }, true);
}

void rfid_report_begin(RfidApp* app) {
    text_box_reset(app->text_box); // drop the stale text pointer before rebuilding
    furi_string_reset(app->info_text);
}

void rfid_report_show(RfidApp* app, const char* header) {
    // No actions submenu in this read-only app (unlike universal_card_reader's
    // Save/Emulate/Rescan/Exit), so there is no header widget to feed; the
    // card name is already the first line of the report body. Accepted for a
    // stable/symmetrical API and possible future use.
    UNUSED(header);
    furi_string_cat_str(app->info_text, "\n[Back] = menu\n");
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_focus(app->text_box, TextBoxFocusStart);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->info_text));
    rfid_switch_view(app, RfidViewInfo);
}
