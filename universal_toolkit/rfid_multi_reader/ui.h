#pragma once

#include "rfid_app.h"

void rfid_draw_callback(Canvas* canvas, void* model);

// The only file allowed to call with_view_model(); everything else goes
// through these wrappers.
void rfid_set_state(RfidApp* app, RfidState state); // writes app->state AND model.state
void rfid_set_scanning(RfidApp* app, const char* band, const char* mode); // labels only
void rfid_set_notice(RfidApp* app, const char* title, const char* l1, const char* l2);
void rfid_bump_frame(RfidApp* app);

// Shared head/tail of the report: begin -> backend describe() (caller) -> show.
void rfid_report_begin(RfidApp* app); // text_box_reset + furi_string_reset
void rfid_report_show(RfidApp* app, const char* header); // footer + set_text + switch view
