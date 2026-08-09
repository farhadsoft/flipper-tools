#pragma once

#include <gui/canvas.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------- design tokens (invariant #7) ------------------- */

#define UI_W             128
#define UI_H              64
#define UI_STATUSBAR_H    12
#define UI_RULE_Y         12
#define UI_CONTENT_TOP    15
#define UI_FOOTER_Y       51
#define UI_MARGIN          4
#define UI_MARGIN_WIDE     6
#define UI_METER_H         6
#define UI_METER_SEG       8

/* ---------------------------- status bar flags ----------------------------- */

typedef enum {
    UiStatusRecording = (1u << 0), // filled disc, blinks with phase
    UiStatusArmed     = (1u << 1), // hollow circle
    UiStatusScanning  = (1u << 2), // scan sweep chevron
} UiStatus;

/* ----------------------------- shared chrome ------------------------------- */

// Inverted status bar + rule at UI_RULE_Y.
// left_label  -- left-aligned after the state glyph on the left.
// center_label -- centered in the bar. If NULL/empty, left_label is
//                 centered instead (current title-bar behaviour).
// flags       -- picks the state glyph: REC (blinks), armed, or scan.
// battery_pct -- 0-100; draws a filled battery icon on the right.
// phase       -- animation phase counter; drives REC blink.
void ui_status_bar(
    Canvas* canvas,
    const char* left_label,
    const char* center_label,
    uint8_t flags,
    uint8_t battery_pct,
    uint8_t phase);

// Hero value in FontBigNumbers + FontSecondary unit, baseline-aligned.
// Exactly one per screen -- the single most important number.
void ui_hero(Canvas* canvas, int x, int y, const char* value, const char* unit);

// Segmented meter bar with threshold tick and pulsing leading edge.
// level_0_255  -- current fill (0 = empty, 255 = full).
// thresh_0_255 -- threshold/trigger tick position.
// phase        -- animation phase; drives the leading-edge pulse.
void ui_meter(
    Canvas* canvas,
    int x,
    int y,
    int w,
    uint8_t level_0_255,
    uint8_t thresh_0_255,
    uint8_t phase);

// Rounded-frame pill for a mode tag ("AM650", "125 kHz RFID", etc.).
// Sits at (x, y); width is automatic from the label.
void ui_chip(Canvas* canvas, int x, int y, const char* label);

// Footer row: glyph + number for saved, dropped, and dup.
// dup is drawn only when > 0; the space collapses otherwise.
void ui_footer_counts(
    Canvas* canvas, uint32_t saved, uint32_t dropped, uint32_t dup);

// Centered notice/confirm frame: FontPrimary title, two FontSecondary body
// lines, wrapped in a rounded frame. Unifies every existing per-module notice.
void ui_notice(Canvas* canvas, const char* title, const char* l1, const char* l2);

/* ------------------------------- helpers ----------------------------------- */

// Center `str` horizontally in the full canvas width at baseline `y`.
void ui_draw_centered(Canvas* canvas, int y, const char* str);

// Like ui_draw_centered() but truncates with ".." until the string fits
// inside `max` pixels.
void ui_draw_centered_fit(Canvas* canvas, int y, const char* str, int max);
