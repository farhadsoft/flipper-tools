#pragma once

#include "reader_app.h"

void reader_draw_callback(Canvas* canvas, void* model);
void reader_set_scanning(ReaderApp* app, bool lf);
void reader_enter_emulating(ReaderApp* app, const char* type);

// The only file allowed to call with_view_model(); everything else goes
// through these wrappers.
void reader_set_state(ReaderApp* app, ReaderState state);
ReaderState reader_get_state(ReaderApp* app);
void reader_bump_frame(ReaderApp* app);
void reader_set_notice(ReaderApp* app, const char* title, const char* l1, const char* l2);

// Shared head/tail of the NFC/LF read report: begin -> card_info_format_*()
// (caller) -> show.
void reader_report_begin(ReaderApp* app);
void reader_report_show(ReaderApp* app, const char* header);
