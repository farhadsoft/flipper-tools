#pragma once

#include "recorder_app.h"

void sub_rec_draw_callback(Canvas* canvas, void* model);

// recorder_ui.c is the ONLY file that calls with_view_model(); every other
// module goes through these setters.
void sub_rec_set_rssi(SubRecApp* app, float rssi, bool update);
void sub_rec_set_samples(SubRecApp* app, size_t samples);
// Sole writer of app->state (and app->cooldown): assigns both fields and
// mirrors them into the model in one call. Nothing else may assign
// app->state or app->cooldown directly, or the on-screen state desyncs.
// May be called with an unchanged `s` purely to change `cooldown`.
void sub_rec_set_state(SubRecApp* app, SubRecState s, bool cooldown);
void sub_rec_set_notice(SubRecApp* app, const char* title, const char* l1, const char* l2, bool active);
void sub_rec_set_counts(SubRecApp* app, uint32_t saved, uint32_t dropped, uint32_t dup, const char* last_file);
// Single-field setter for the session dup reset in sub_rec_listen_start() --
// unlike sub_rec_set_counts(), this must not disturb last_file, which stays
// meaningful (the last capture saved) across an arm/disarm cycle.
void sub_rec_set_dup(SubRecApp* app, uint32_t dup);
void sub_rec_set_proto_line(SubRecApp* app, const char* proto);
// Sole writer of the model's `trigger` (the RSSI-bar tick position).
void sub_rec_set_freq_line(SubRecApp* app, const char* line, float trigger);
void sub_rec_set_scan(SubRecApp* app, uint8_t idx, int8_t dbm, uint8_t peak, bool update);
// Floor-fills scan_dbm[] and points scan_peak at the current frequency, so the
// first frame of a scan is a flat floor instead of the previous sweep.
void sub_rec_reset_scan(SubRecApp* app);

// Publishes one parsed capture. update=false: sub_rec_set_analyze_page() repaints
// immediately after, and repainting here would draw new data under the old page.
void sub_rec_set_analyze(SubRecApp* app, const SubRecAnalysis* a);
// Sole writer of app->ana_page and the model's copy -- same pairing as
// sub_rec_set_state(). 0 = info, 1 = waveform.
void sub_rec_set_analyze_page(SubRecApp* app, uint8_t page);
// Sole writer of the model's window trio (win_start_us/win_us/zoom) and the
// windowed waveform buffer.
void sub_rec_set_analyze_window(
    SubRecApp* app, uint32_t start_us, uint32_t win_us, uint8_t zoom, const uint8_t* wave);
void sub_rec_set_stats(SubRecApp* app, const SubRecStats* s);
