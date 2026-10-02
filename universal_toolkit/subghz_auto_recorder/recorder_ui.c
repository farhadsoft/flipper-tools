#include "recorder_ui.h"
#include "../toolkit_ui.h"
#include <stdio.h>
#include <string.h>

// Scan bar graph: one bar per sub_rec_freqs[] entry, centred.
#define SCAN_BAR_W     6
#define SCAN_BAR_PITCH 7

#define SCAN_X0        ((UI_W - ((int)COUNT_OF(sub_rec_freqs) * SCAN_BAR_PITCH - 1)) / 2)
#define SCAN_BASE_Y    52 // bars occupy rows SCAN_BASE_Y-h .. SCAN_BASE_Y-1
#define SCAN_MAX_H     34

// Analyze waveform: a baseline across the band plus a full-height column
// wherever the level is high, so a run of high columns reads as a solid block --
// the classic OOK burst shape on a 1-bit screen.
#define WAVE_X0   4
#define WAVE_Y_HI 24
#define WAVE_Y_LO 46



static float clamp01(float f) {
    if(f < 0.0f) return 0.0f;
    if(f > 1.0f) return 1.0f;
    return f;
}

static void draw_listening(Canvas* canvas, const SubRecModel* m) {
    bool recording = (m->state == SubRecStateRecording);
    ui_status_bar(
        canvas,
        recording ? "RECORDING" : "Listening",
        m->freq_line,
        recording ? UiStatusRecording : 0,
        0,
        m->anim_phase);

    float rssi = m->rssi;
    if(rssi < RSSI_FLOOR_DBM) rssi = RSSI_FLOOR_DBM;
    char rssi_str[8];
    snprintf(rssi_str, sizeof(rssi_str), "%.0f", (double)rssi);
    ui_hero(canvas, UI_MARGIN, 30, rssi_str, "dBm");

    canvas_set_font(canvas, FontBigNumbers);
    int hero_w = canvas_string_width(canvas, rssi_str);
    canvas_set_font(canvas, FontSecondary);
    const char* mod_label = strrchr(m->freq_line, ' ');
    mod_label = mod_label ? mod_label + 1 : m->freq_line;
    ui_chip(canvas, UI_MARGIN + hero_w + 6, 19, mod_label);

    uint8_t level =
        (uint8_t)(clamp01((rssi - RSSI_FLOOR_DBM) / (RSSI_CEIL_DBM - RSSI_FLOOR_DBM)) * 255);
    uint8_t thresh =
        (uint8_t)(clamp01((m->trigger - RSSI_FLOOR_DBM) / (RSSI_CEIL_DBM - RSSI_FLOOR_DBM)) * 255);
    ui_meter(canvas, UI_MARGIN, 32, UI_W - 2 * UI_MARGIN, level, thresh, m->anim_phase);

    // Two lines while a live decode is up: the firmware's own "<name>
    // <bits>bit" plus its key hex. y=45/54 keeps the pair clear of the meter
    // above (rows 32-38) and of the footer glyphs below (centre y=58).
    char buf[32];
    if(recording) {
        snprintf(buf, sizeof(buf), "rec  %u spl", (unsigned)m->samples);
        ui_draw_centered(canvas, 50, buf);
    } else if(m->cooldown) {
        ui_draw_centered(canvas, 50, "carrier");
    } else if(m->live_key[0]) {
        ui_draw_centered_fit(canvas, 45, m->proto_line, 124);
        snprintf(buf, sizeof(buf), "Key %s", m->live_key);
        ui_draw_centered_fit(canvas, 54, buf, 124);
    } else if(m->proto_line[0]) {
        ui_draw_centered_fit(canvas, 50, m->proto_line, 124);
    } else {
        ui_draw_centered(canvas, 50, "armed");
    }

    ui_footer_counts(canvas, m->saved, m->dropped, m->dup);
}

static void draw_sending(Canvas* canvas, const SubRecModel* m) {
    ui_status_bar(canvas, "Sending", NULL, 0, 0, 0);
    ui_draw_centered_fit(canvas, 32, m->last_file, 120);
    // No animation timer exists in this app (see CLAUDE.md); the dots are a
    // static "in progress" decoration, not a cycling animation.
    ui_draw_centered(canvas, 44, "...");
}

// Frequency-scan status screen: a 17-bar RSSI graph (one bar per
// sub_rec_freqs[] entry), a peak marker, and the peak's frequency + dBm.
// Shares draw_listening()'s dBm->pixel mapping (RSSI_FLOOR_DBM/CEIL_DBM,
// clamp01()) so the two screens read on the same scale.
static void draw_scanning(Canvas* canvas, const SubRecModel* m) {
    ui_status_bar(canvas, "Scan", "OK=tune", UiStatusScanning, 0, m->anim_phase);

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

    // Sweep marker: small downward caret above the bar currently being scanned.
    int sx = SCAN_X0 + (int)m->scan_idx * SCAN_BAR_PITCH;
    canvas_draw_line(
        canvas, sx - 2, SCAN_BASE_Y - SCAN_MAX_H - 2, sx, SCAN_BASE_Y - SCAN_MAX_H + 1);
    canvas_draw_line(
        canvas, sx + 2, SCAN_BASE_Y - SCAN_MAX_H - 2, sx, SCAN_BASE_Y - SCAN_MAX_H + 1);

    uint32_t f = sub_rec_freqs[m->scan_peak];
    char freq_str[8];
    snprintf(freq_str, sizeof(freq_str), "%lu", (unsigned long)(f / 1000000));
    ui_hero(canvas, UI_MARGIN, 48, freq_str, "MHz");

    char buf[32];
    snprintf(
        buf,
        sizeof(buf),
        "%lu.%02lu MHz  %d dBm",
        (unsigned long)(f / 1000000),
        (unsigned long)(f / 10000 % 100),
        (int)m->scan_dbm[m->scan_peak]);
    ui_draw_centered(canvas, 63, buf);
}

static const char* const sub_rec_zoom_labels[] = {"FIT", "x2", "x4", "x8", "ALL"};

static void draw_analyzing(Canvas* canvas, const SubRecModel* m) {
    char buf[40];

    if(m->ana_page == 0) {
        ui_status_bar(canvas, "Info", "v wave", 0, 0, 0);

        char freq_str[8];
        snprintf(freq_str, sizeof(freq_str), "%lu", (unsigned long)(m->ana.freq / 1000000));
        ui_hero(canvas, UI_MARGIN, 26, freq_str, "MHz");

        canvas_set_font(canvas, FontBigNumbers);
        int hero_w = canvas_string_width(canvas, freq_str);
        canvas_set_font(canvas, FontSecondary);
        ui_chip(canvas, UI_MARGIN + hero_w + 6, 16, m->ana.mod);

        snprintf(buf, sizeof(buf), "Proto: %s", m->ana.proto);
        ui_draw_centered_fit(canvas, 34, buf, 124);
        snprintf(
            buf,
            sizeof(buf),
            "%lu spl  %lu B",
            (unsigned long)m->ana.samples,
            (unsigned long)m->ana.bytes);
        ui_draw_centered_fit(canvas, 42, buf, 124);

        bool has_note = m->ana.note[0] != 0;
        bool has_bit = m->ana.bit != 0;
        if(has_note) {
            snprintf(buf, sizeof(buf), "\"%s\"", m->ana.note);
            ui_draw_centered_fit(canvas, 50, buf, 124);
        }
        if(has_bit) {
            snprintf(buf, sizeof(buf), "Bit: %lu  Key: %s", (unsigned long)m->ana.bit, m->ana.key);
            ui_draw_centered_fit(canvas, 60, buf, 124);
        }
        if(!has_note && !has_bit) {
            ui_draw_centered(canvas, 63, "v wave");
        }
        return;
    }

    snprintf(buf, sizeof(buf), "Wave %s  ^info", sub_rec_zoom_labels[m->ana.zoom]);
    ui_status_bar(canvas, buf, NULL, 0, 0, 0);
    if(m->ana.wave_len == 0) {
        ui_draw_centered(canvas, 38, "no samples");
        return;
    }
    canvas_draw_line(canvas, WAVE_X0, WAVE_Y_LO, WAVE_X0 + WAVE_COLS - 1, WAVE_Y_LO);
    for(uint16_t c = 0; c < m->ana.wave_len; c++) {
        if(m->ana.wave[c]) {
            canvas_draw_line(canvas, WAVE_X0 + c, WAVE_Y_HI, WAVE_X0 + c, WAVE_Y_LO);
        }
    }
    // Window start + span, in ms -- the zoom label already says how deep.
    snprintf(
        buf,
        sizeof(buf),
        "%lu.%02lu +%lu.%02lu ms",
        (unsigned long)(m->ana.win_start_us / 1000),
        (unsigned long)(m->ana.win_start_us % 1000 / 10),
        (unsigned long)(m->ana.win_us / 1000),
        (unsigned long)(m->ana.win_us % 1000 / 10));
    ui_draw_centered_fit(canvas, 63, buf, 124);
}

static void draw_stats(Canvas* canvas, const SubRecModel* m) {
    char buf[40];
    ui_status_bar(canvas, "Stats", NULL, 0, 0, 0);

    char files_str[8];
    snprintf(files_str, sizeof(files_str), "%lu", (unsigned long)m->stats.files);
    ui_hero(canvas, UI_MARGIN, 30, files_str, "files");

    canvas_set_font(canvas, FontBigNumbers);
    int hero_w = canvas_string_width(canvas, files_str);
    canvas_set_font(canvas, FontSecondary);
    char kib_str[16];
    snprintf(kib_str, sizeof(kib_str), "%lu KiB", (unsigned long)m->stats.kib);
    ui_chip(canvas, UI_MARGIN + hero_w + 6, 19, kib_str);

    snprintf(
        buf,
        sizeof(buf),
        "raw %lu  dec %lu  rc %lu",
        (unsigned long)m->stats.raw,
        (unsigned long)m->stats.decoded,
        (unsigned long)m->stats.rc);
    ui_draw_centered_fit(canvas, 38, buf, 124);

    ui_footer_counts(canvas, m->stats.saved, m->stats.dropped, m->stats.dup);
}

void sub_rec_draw_callback(Canvas* canvas, void* model) {
    const SubRecModel* m = model;
    canvas_clear(canvas);

    if(m->notice_active) {
        ui_status_bar(canvas, m->notice_title, NULL, 0, 0, 0);
        ui_notice(canvas, NULL, m->notice_l1, m->notice_l2);
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
    case SubRecStateStats:
        draw_stats(canvas, m);
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
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            m->rssi = rssi;
            if(update) m->anim_phase++;
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
            m->anim_phase++;
        },
        true);
}

void sub_rec_set_notice(
    SubRecApp* app, const char* title, const char* l1, const char* l2, bool active) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            snprintf(m->notice_title, sizeof(m->notice_title), "%s", title);
            snprintf(m->notice_l1, sizeof(m->notice_l1), "%s", l1);
            snprintf(m->notice_l2, sizeof(m->notice_l2), "%s", l2);
            m->notice_active = active;
        },
        true);
}

void sub_rec_set_counts(
    SubRecApp* app, uint32_t saved, uint32_t dropped, uint32_t dup, const char* last_file) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            m->saved = saved;
            m->dropped = dropped;
            m->dup = dup;
            snprintf(m->last_file, sizeof(m->last_file), "%s", last_file);
        },
        true);
}

void sub_rec_set_dup(SubRecApp* app, uint32_t dup) {
    with_view_model(app->view, SubRecModel * m, { m->dup = dup; }, true);
}

void sub_rec_set_proto_line(SubRecApp* app, const char* proto) {
    with_view_model(
        app->view,
        SubRecModel * m,
        { snprintf(m->proto_line, sizeof(m->proto_line), "%s", proto); },
        true);
}

void sub_rec_set_live(SubRecApp* app, const char* proto, const char* key) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            snprintf(m->proto_line, sizeof(m->proto_line), "%s", proto);
            snprintf(m->live_key, sizeof(m->live_key), "%s", key);
        },
        true);
}

void sub_rec_set_freq_line(SubRecApp* app, const char* line, float trigger) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            snprintf(m->freq_line, sizeof(m->freq_line), "%s", line);
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
            m->scan_idx = idx;
            // The peak index, not the step index. The old `if(peak)
            // m->scan_peak = idx` wrote the CURRENT sweep step into the
            // model's peak on every tick, so at each repaint the hero
            // frequency and the peak marker sat on the last table entry and
            // never changed while the bars swept.
            m->scan_peak = peak;
        },
        update);
}

void sub_rec_reset_scan(SubRecApp* app) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            for(size_t i = 0; i < COUNT_OF(sub_rec_freqs); i++) m->scan_dbm[i] = RSSI_FLOOR_DBM;
            m->scan_peak = 0;
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

void sub_rec_set_stats(SubRecApp* app, const SubRecStats* s) {
    with_view_model(app->view, SubRecModel * m, { m->stats = *s; }, false);
}

void sub_rec_set_analyze_window(
    SubRecApp* app, uint32_t start_us, uint32_t win_us, uint8_t zoom, const uint8_t* wave) {
    with_view_model(
        app->view,
        SubRecModel * m,
        {
            m->ana.win_start_us = start_us;
            m->ana.win_us = win_us;
            m->ana.zoom = zoom;
            m->ana.wave_len = WAVE_COLS;
            memcpy(m->ana.wave, wave, WAVE_COLS);
        },
        true);
}
