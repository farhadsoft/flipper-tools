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
void sub_rec_set_counts(SubRecApp* app, uint32_t saved, uint32_t dropped, const char* last_file);
// Sole writer of the model's `trigger` (the RSSI-bar tick position).
void sub_rec_set_freq_line(SubRecApp* app, const char* line, float trigger);
