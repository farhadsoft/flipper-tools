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

// Scan bar graph: one bar per sub_rec_freqs[] entry, centred.
#define SCAN_BAR_W     6
#define SCAN_BAR_PITCH 7
#define SCAN_X0        ((SCREEN_W - ((int)COUNT_OF(sub_rec_freqs) * SCAN_BAR_PITCH - 1)) / 2)
#define SCAN_BASE_Y    52 // bars occupy rows SCAN_BASE_Y-h .. SCAN_BASE_Y-1
#define SCAN_MAX_H     34

// Analyze waveform: a baseline across the band plus a full-height column
// wherever the level is high, so a run of high columns reads as a solid block --
// the classic OOK burst shape on a 1-bit screen.
#define WAVE_X0   4
#define WAVE_Y_HI 24
#define WAVE_Y_LO 46

static void draw_centered(Canvas* canvas, int cy, const char* str) {
    int w = canvas_string_width(canvas, str);
    canvas_draw_str(canvas, (SCREEN_W - w) / 2, cy, str);
}

// Like draw_centered(), but truncates with ".." until the string fits inside
// `max` pixels, so a long file name never runs off-screen.
static void draw_centered_fit(Canvas* canvas, int cy, const char* str, int max) {
    char buf[REC_TEXT_LINE_MAX];
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
    } else if(m->proto_line[0]) {
        draw_centered_fit(canvas, 42, m->proto_line, 124);
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

// Frequency-scan status screen: a 17-bar RSSI graph (one bar per
// sub_rec_freqs[] entry), a peak marker, and the peak's frequency + dBm.
// Shares draw_rssi_bar()'s dBm->pixel mapping (RSSI_FLOOR_DBM/CEIL_DBM,
// clamp01()) so the two screens read on the same scale.
static void draw_scanning(Canvas* canvas, const SubRecModel* m) {
    draw_title_bar(canvas, "Scan   OK=tune");

    canvas_draw_line(
        canvas,
        SCAN_X0,
        SCAN_BASE_Y,
        SCAN_X0 + (int)COUNT_OF(sub_rec_freqs) * SCAN_BAR_PITCH - 2,
        SCAN_BASE_Y);

    for(size_t i = 0; i < COUNT_OF(sub_rec_freqs); i++) {
        int h = (int)(
            clamp01(((float)m->scan_dbm[i] - RSSI_FLOOR_DBM) / (RSSI_CEIL_DBM - RSSI_FLOOR_DBM)) *
            SCAN_MAX_H);
        if(h > 0) {
            canvas_draw_box(
                canvas, SCAN_X0 + (int)i * SCAN_BAR_PITCH, SCAN_BASE_Y - h, SCAN_BAR_W, h);
        }
    }

    canvas_draw_box(
        canvas, SCAN_X0 + (int)m->scan_peak * SCAN_BAR_PITCH + 1, SCAN_BASE_Y + 2, 4, 2);

    uint32_t f = sub_rec_freqs[m->scan_peak];
    char buf[32];
    snprintf(
        buf,
        sizeof(buf),
        "%lu.%02lu MHz  %d dBm",
        (unsigned long)(f / 1000000),
        (unsigned long)(f / 10000 % 100),
        (int)m->scan_dbm[m->scan_peak]);
    draw_centered(canvas, 63, buf);
}

static void draw_analyzing(Canvas* canvas, const SubRecModel* m) {
    char buf[40];

    if(m->ana_page == 0) {
        draw_title_bar(canvas, "Info  >waveform");
        // Same "%lu.%02lu MHz" integer split every other screen in this app uses;
        // no float printf is dragged in.
        snprintf(
            buf,
            sizeof(buf),
            "%lu.%02lu MHz  %s",
            (unsigned long)(m->ana.freq / 1000000),
            (unsigned long)(m->ana.freq / 10000 % 100),
            m->ana.mod);
        draw_centered_fit(canvas, 24, buf, 124);
        snprintf(buf, sizeof(buf), "Proto: %s", m->ana.proto);
        draw_centered_fit(canvas, 34, buf, 124);
        snprintf(
            buf,
            sizeof(buf),
            "%lu spl  %lu B",
            (unsigned long)m->ana.samples,
            (unsigned long)m->ana.bytes);
        draw_centered_fit(canvas, 44, buf, 124);
        if(m->ana.note[0]) {
            snprintf(buf, sizeof(buf), "\"%s\"", m->ana.note);
            draw_centered_fit(canvas, 54, buf, 124);
        }
        if(m->ana.bit) {
            snprintf(buf, sizeof(buf), "Bit: %lu  Key: %s", (unsigned long)m->ana.bit, m->ana.key);
            draw_centered_fit(canvas, 63, buf, 124);
        }
        return;
    }

    draw_title_bar(canvas, "Waveform  >info");
    if(m->ana.wave_len == 0) {
        draw_centered(canvas, 38, "no samples");
        return;
    }
    canvas_draw_line(canvas, WAVE_X0, WAVE_Y_LO, WAVE_X0 + WAVE_COLS - 1, WAVE_Y_LO);
    for(uint16_t c = 0; c < m->ana.wave_len; c++) {
        if(m->ana.wave[c]) {
            canvas_draw_line(canvas, WAVE_X0 + c, WAVE_Y_HI, WAVE_X0 + c, WAVE_Y_LO);
        }
    }
    snprintf(
        buf,
        sizeof(buf),
        "%lu spl  %lu.%02lu ms",
        (unsigned long)m->ana.samples,
        (unsigned long)(m->ana.total_us / 1000),
        (unsigned long)(m->ana.total_us % 1000 / 10));
    draw_centered_fit(canvas, 63, buf, 124);
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
    case SubRecStateScanning:
        draw_scanning(canvas, m);
        break;
    case SubRecStateAnalyzing:
        draw_analyzing(canvas, m);
        break;
    case SubRecStateIdle:
    default:
        break;
    }
}

void sub_rec_set_rssi(SubRecApp* app, float rssi, bool update) {
    with_view_model(app->view, SubRecModel * m, { m->rssi = rssi; }, update);
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

void sub_rec_set_proto_line(SubRecApp* app, const char* proto) {
    with_view_model(
        app->view,
        SubRecModel * m,
        { snprintf(m->proto_line, sizeof(m->proto_line), "%s", proto); },
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

void sub_rec_set_scan(SubRecApp* app, uint8_t idx, int8_t dbm, uint8_t peak, bool update) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            m->scan_dbm[idx] = dbm;
            m->scan_peak = peak;
        },
        update);
}

void sub_rec_reset_scan(SubRecApp* app) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            memset(m->scan_dbm, (int8_t)RSSI_FLOOR_DBM, sizeof(m->scan_dbm));
            m->scan_peak = app->freq_idx;
        },
        true);
}

void sub_rec_set_analyze(SubRecApp* app, const SubRecAnalysis* a) {
    with_view_model(app->view, SubRecModel * m, { m->ana = *a; }, false);
}

void sub_rec_set_analyze_page(SubRecApp* app, uint8_t page) {
    app->ana_page = page;
    with_view_model(app->view, SubRecModel * m, { m->ana_page = page; }, true);
}
