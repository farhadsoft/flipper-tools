#pragma once

#include <gui/canvas.h>

// Shared chrome, deliberately minimal: Phase 0 needs only the title bar
// (the launcher's own header comes from Submenu). Specialised drawing stays
// in each module -- this file grows only when a second module needs the
// same widget, not in anticipation of one.
void toolkit_ui_draw_title_bar(Canvas* canvas, const char* title);
