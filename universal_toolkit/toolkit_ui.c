#include "toolkit_ui.h"

#include <gui/canvas.h>
#include <stdio.h>
#include <string.h>

/* ----------------------------- static helpers ------------------------------ */

// Value-or-default: returns `val` unless it is NULL or the empty string,
// in which case `def` is returned instead.
static const char* or_default(const char* val, const char* def) {
    return (val && val[0]) ? val : def;
}

/* -------------------------- status bar glyphs ------------------------------ */

// Blinking filled disc for REC.  frame 0-3 on, 4-7 off -> ~50 % duty.
static void draw_rec_glyph(Canvas* canvas, int x, int y, uint8_t phase) {
    if((phase & 4) == 0) canvas_draw_disc(canvas, x, y, 2);
}

// Hollow circle for armed.
static void draw_armed_glyph(Canvas* canvas, int x, int y) {
    canvas_draw_circle(canvas, x, y, 2);
}

// Small chevron / sweep marker for scanning.
static void draw_scan_glyph(Canvas* canvas, int x, int y) {
    canvas_draw_line(canvas, x - 3, y - 3, x, y);
    canvas_draw_line(canvas, x - 3, y + 3, x, y);
}

// Small blinking dot for live/monitoring.  frame 0-3 on, 4-7 off.
static void draw_live_glyph(Canvas* canvas, int x, int y, uint8_t phase) {
    if((phase & 4) == 0) canvas_draw_disc(canvas, x, y, 1);
}

// Bluetooth rune: vertical stem + two chevron halves, ~5x6 px.
static void draw_ble_glyph(Canvas* canvas, int x, int y) {
    canvas_draw_line(canvas, x, y - 3, x, y + 3); // stem
    canvas_draw_line(canvas, x, y - 3, x - 2, y - 1); // upper left
    canvas_draw_line(canvas, x, y - 3, x + 2, y - 1); // upper right
    canvas_draw_line(canvas, x, y + 3, x - 2, y + 1); // lower left
    canvas_draw_line(canvas, x, y + 3, x + 2, y + 1); // lower right
}

// Battery icon: frame + nub + proportional fill.
// Frame is 10x6; nub is 2x3 to the right. Fill width = batt * 8 / 100.
static void draw_battery(Canvas* canvas, int x, int y, uint8_t pct) {
    int bx = x, by = y + 3;
    // outline
    canvas_draw_frame(canvas, bx, by, 10, 6);
    // nub
    canvas_draw_box(canvas, bx + 10, by + 1, 2, 4);
    // fill -- internal area is 8x4 (1 px padding inside the frame)
    int fw = (int)pct * 8 / 100;
    if(fw > 0) canvas_draw_box(canvas, bx + 1, by + 1, fw, 4);
}

/* ---------------------------- footer glyphs --------------------------------- */

// Down-arrow into a tray: saved icon.  6 px wide, 6 px tall, centred on (gx, gy).
static void draw_saved_glyph(Canvas* canvas, int gx, int gy) {
    // vertical stem
    canvas_draw_line(canvas, gx, gy - 3, gx, gy - 1);
    // left arrowhead
    canvas_draw_line(canvas, gx - 2, gy - 1, gx, gy - 3);
    // right arrowhead
    canvas_draw_line(canvas, gx + 2, gy - 1, gx, gy - 3);
    // tray baseline
    canvas_draw_line(canvas, gx - 3, gy + 1, gx + 3, gy + 1);
}

// Cross (×): dropped icon.  5 px × 5 px, centred on (gx, gy).
static void draw_dropped_glyph(Canvas* canvas, int gx, int gy) {
    canvas_draw_line(canvas, gx - 2, gy - 2, gx + 2, gy + 2);
    canvas_draw_line(canvas, gx - 2, gy + 2, gx + 2, gy - 2);
}

/* ---------------------------- shared chrome -------------------------------- */

void ui_status_bar(
    Canvas* canvas,
    const char* left_label,
    const char* center_label,
    uint8_t flags,
    uint8_t battery_pct,
    uint8_t phase) {
    // Inverted fill: 0..UI_STATUSBAR_H-1
    canvas_draw_box(canvas, 0, 0, UI_W, UI_STATUSBAR_H);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);

    // -- state glyph on the left --
    int text_x = UI_MARGIN;
    if(flags & UiStatusRecording) {
        draw_rec_glyph(canvas, UI_MARGIN + 3, 6, phase);
        text_x = UI_MARGIN + 8;
    } else if(flags & UiStatusArmed) {
        draw_armed_glyph(canvas, UI_MARGIN + 3, 6);
        text_x = UI_MARGIN + 8;
    } else if(flags & UiStatusScanning) {
        draw_scan_glyph(canvas, UI_MARGIN + 3, 6);
        text_x = UI_MARGIN + 8;
    } else if(flags & UiStatusLive) {
        draw_live_glyph(canvas, UI_MARGIN + 3, 6, phase);
        text_x = UI_MARGIN + 8;
    } else if(flags & UiStatusBle) {
        draw_ble_glyph(canvas, UI_MARGIN + 3, 6);
        text_x = UI_MARGIN + 8;
    }

    // -- text --
    if(center_label && center_label[0]) {
        // Two-label mode: left label after glyph, center label centered.
        // If the centered label is too long, drop the left label to avoid overlap.
        const char* ll = left_label ? left_label : "";
        int lw = canvas_string_width(canvas, ll);
        int cw = canvas_string_width(canvas, center_label);
        int cx = (UI_W - cw) / 2;
        if(text_x + lw + 4 > cx) {
            // Collision: show center label only.
            canvas_draw_str(canvas, cx, 10, center_label);
        } else {
            canvas_draw_str(canvas, text_x, 10, ll);
            canvas_draw_str(canvas, cx, 10, center_label);
        }
    } else {
        // Single-label mode: center the left label (current title-bar behaviour).
        const char* s = or_default(left_label, "");
        int w = canvas_string_width(canvas, s);
        canvas_draw_str(canvas, (UI_W - w) / 2, 10, s);
    }

    // -- battery on the right --
    if(battery_pct <= 100) draw_battery(canvas, UI_W - UI_MARGIN - 12, 0, battery_pct);

    // restore
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    // rule at UI_RULE_Y
    canvas_draw_line(canvas, 0, UI_RULE_Y, UI_W - 1, UI_RULE_Y);
}

void ui_hero(Canvas* canvas, int x, int y, const char* value, const char* unit) {
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, x, y, value);

    if(unit && unit[0]) {
        int vw = canvas_string_width(canvas, value);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, x + vw + 3, y, unit);
    }

    canvas_set_font(canvas, FontSecondary);
}

void ui_meter(
    Canvas* canvas,
    int x,
    int y,
    int w,
    uint8_t level_0_255,
    uint8_t thresh_0_255,
    uint8_t phase) {
    int pitch = UI_METER_SEG + 1; // 8 px segment + 1 px gap
    int n_segs = (w - 1) / pitch;
    if(n_segs < 1) return;

    int filled = (int)level_0_255 * n_segs / 255;
    bool pulse_on = (phase & 4) != 0;

    for(int i = 0; i < n_segs; i++) {
        int sx = x + i * pitch;
        if(i < filled) {
            canvas_draw_box(canvas, sx, y, UI_METER_SEG, UI_METER_H);
        } else if(i == filled && pulse_on) {
            // pulsing leading edge -- filled this frame, empty next
            canvas_draw_box(canvas, sx, y, UI_METER_SEG, UI_METER_H);
        }
        // segments past the fill level are left blank (no outline)
    }

    // threshold tick -- below the bar, since a black tick on black fill is invisible
    int tick_x = x + (int)thresh_0_255 * (n_segs * pitch - 1) / 255;
    canvas_draw_line(canvas, tick_x, y + UI_METER_H + 1, tick_x, y + UI_METER_H + 3);
}

void ui_chip(Canvas* canvas, int x, int y, const char* label) {
    if(!label || !label[0]) return;

    canvas_set_font(canvas, FontSecondary);
    int lw = canvas_string_width(canvas, label);
    int pad = 3;
    canvas_draw_rframe(canvas, x, y, lw + pad * 2, 9, 2);
    canvas_draw_str(canvas, x + pad, y + 7, label);
}

void ui_footer_counts(
    Canvas* canvas, uint32_t saved, uint32_t dropped, uint32_t dup) {
    canvas_set_font(canvas, FontSecondary);

    char buf[16];
    int gy = UI_FOOTER_Y + 7; // glyph vertical centre
    int ty = UI_FOOTER_Y + 11; // text baseline

    // -- saved --
    int x = UI_MARGIN;
    draw_saved_glyph(canvas, x + 3, gy);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)saved);
    canvas_draw_str(canvas, x + 9, ty, buf);
    int w = canvas_string_width(canvas, buf);

    // -- dropped --
    x += 9 + w + 10;
    draw_dropped_glyph(canvas, x + 3, gy);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)dropped);
    canvas_draw_str(canvas, x + 9, ty, buf);
    w = canvas_string_width(canvas, buf);

    // -- dup (only when > 0) --
    if(dup > 0) {
        x += 9 + w + 10;
        snprintf(buf, sizeof(buf), "dup %lu", (unsigned long)dup);
        canvas_draw_str(canvas, x, ty, buf);
    }
}

void ui_notice(Canvas* canvas, const char* title, const char* l1, const char* l2) {
    // measure to size the frame
    canvas_set_font(canvas, FontPrimary);
    int tw = canvas_string_width(canvas, title ? title : "");
    canvas_set_font(canvas, FontSecondary);
    int w1 = canvas_string_width(canvas, l1 ? l1 : "");
    int w2 = canvas_string_width(canvas, l2 ? l2 : "");

    int fw = tw;
    if(w1 > fw) fw = w1;
    if(w2 > fw) fw = w2;
    fw += 16; // 8 px padding each side
    if(fw > UI_W - 8) fw = UI_W - 8;
    int fx = (UI_W - fw) / 2;

    // frame: from y=17 to y=56
    canvas_draw_rframe(canvas, fx, 17, fw, 40, 3);

    // title
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, (UI_W - tw) / 2, 28, title ? title : "");

    // body
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, (UI_W - w1) / 2, 41, l1 ? l1 : "");
    canvas_draw_str(canvas, (UI_W - w2) / 2, 52, l2 ? l2 : "");
}

/* ------------------------------- helpers ----------------------------------- */

void ui_draw_centered(Canvas* canvas, int y, const char* str) {
    int w = canvas_string_width(canvas, str);
    canvas_draw_str(canvas, (UI_W - w) / 2, y, str);
}

void ui_draw_centered_fit(Canvas* canvas, int y, const char* str, int max) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", str ? str : "");
    size_t len = strlen(buf);
    while(len > 3 && canvas_string_width(canvas, buf) > max) {
        buf[--len] = '\0';
        buf[len - 1] = '.';
        buf[len - 2] = '.';
    }
    ui_draw_centered(canvas, y, buf);
}
