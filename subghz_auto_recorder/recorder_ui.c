#include "recorder_ui.h"
#include <stdio.h>
#include <string.h>

#define SCREEN_W    128
#define TITLE_BAR_H 13

// RSSI bar geometry -- shared between the fill and the trigger tick so they
// stay aligned however the numbers above change.
#define BAR_X 4
#define BAR_Y 26
#define BAR_W 120
#define BAR_H 8

static void draw_centered(Canvas* canvas, int cy, const char* str) {
    int w = canvas_string_width(canvas, str);
    canvas_draw_str(canvas, (SCREEN_W - w) / 2, cy, str);
}

// Like draw_centered(), but truncates with "..." until the string fits inside
// `max` pixels, so a long file name never runs off-screen.
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

static float clamp01(float f) {
    if(f < 0.0f) return 0.0f;
    if(f > 1.0f) return 1.0f;
    return f;
}

// RSSI bar: an outline frame, an inset fill proportional to how close `rssi`
// is to RSSI_CEIL_DBM, and a tick mark showing the trigger threshold. The
// tick is drawn just below the frame rather than through the fill, since a
// black tick on top of a black fill would be invisible whenever the signal
// is already above the trigger.
static void draw_rssi_bar(Canvas* canvas, float rssi, float trigger) {
    canvas_draw_frame(canvas, BAR_X, BAR_Y, BAR_W, BAR_H);

    float frac = clamp01((rssi - RSSI_FLOOR_DBM) / (RSSI_CEIL_DBM - RSSI_FLOOR_DBM));
    size_t fill = (size_t)(frac * (BAR_W - 2));
    if(fill > 0) canvas_draw_box(canvas, BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2);

    float tfrac = clamp01((trigger - RSSI_FLOOR_DBM) / (RSSI_CEIL_DBM - RSSI_FLOOR_DBM));
    int32_t tick_x = BAR_X + 1 + (int32_t)(tfrac * (BAR_W - 2));
    canvas_draw_line(canvas, tick_x, BAR_Y + BAR_H + 1, tick_x, BAR_Y + BAR_H + 3);
}

static void draw_listening(Canvas* canvas, const SubRecModel* m) {
    draw_title_bar(canvas, m->state == SubRecStateRecording ? "RECORDING" : "Listening");
    draw_centered(canvas, 22, m->freq_line);
    draw_rssi_bar(canvas, m->rssi, m->trigger);

    char buf[32];
    if(m->state == SubRecStateRecording) {
        snprintf(buf, sizeof(buf), "rec  %u spl", (unsigned)m->samples);
        draw_centered(canvas, 42, buf);
    } else if(m->cooldown) {
        draw_centered(canvas, 42, "carrier");
    } else {
        draw_centered(canvas, 42, "armed");
    }

    snprintf(
        buf, sizeof(buf), "saved %lu   drop %lu", (unsigned long)m->saved, (unsigned long)m->dropped);
    draw_centered(canvas, 53, buf);

    draw_centered_fit(canvas, 63, m->last_file, 120);
}

static void draw_sending(Canvas* canvas, const SubRecModel* m) {
    draw_title_bar(canvas, "Sending");
    draw_centered_fit(canvas, 32, m->last_file, 120);
    // No animation timer exists in this app (see CLAUDE.md); the dots are a
    // static "in progress" decoration, not a cycling animation.
    draw_centered(canvas, 44, "...");
}

void sub_rec_draw_callback(Canvas* canvas, void* model) {
    const SubRecModel* m = model;
    canvas_clear(canvas);

    if(m->notice_active) {
        draw_title_bar(canvas, m->notice_title);
        draw_centered_fit(canvas, 34, m->notice_l1, 124);
        draw_centered_fit(canvas, 48, m->notice_l2, 124);
        return;
    }

    switch(m->state) {
    case SubRecStateArmed:
    case SubRecStateRecording:
        draw_listening(canvas, m);
        break;
    case SubRecStateSending:
        draw_sending(canvas, m);
        break;
    case SubRecStateIdle:
    default:
        break;
    }
}

void sub_rec_set_rssi(SubRecApp* app, float rssi, bool above, bool update) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            m->rssi = rssi;
            m->above = above;
        },
        update);
}

void sub_rec_set_samples(SubRecApp* app, size_t samples) {
    with_view_model(app->view, SubRecModel * m, { m->samples = samples; }, true);
}

void sub_rec_set_state(SubRecApp* app, SubRecState s, bool cooldown) {
    app->state = s;
    app->cooldown = cooldown;
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            m->state = s;
            m->cooldown = cooldown;
        },
        true);
}

void sub_rec_set_notice(SubRecApp* app, const char* title, const char* l1, const char* l2, bool active) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            m->notice_active = active;
            snprintf(m->notice_title, sizeof(m->notice_title), "%s", title ? title : "");
            snprintf(m->notice_l1, sizeof(m->notice_l1), "%s", l1 ? l1 : "");
            snprintf(m->notice_l2, sizeof(m->notice_l2), "%s", l2 ? l2 : "");
        },
        true);
}

void sub_rec_set_counts(SubRecApp* app, uint32_t saved, uint32_t dropped, const char* last_file) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            m->saved = saved;
            m->dropped = dropped;
            snprintf(m->last_file, sizeof(m->last_file), "%s", last_file ? last_file : "");
        },
        true);
}

void sub_rec_set_freq_line(SubRecApp* app, const char* line, float trigger) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            snprintf(m->freq_line, sizeof(m->freq_line), "%s", line ? line : "");
            m->trigger = trigger;
        },
        true);
}
