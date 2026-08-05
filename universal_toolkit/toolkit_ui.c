#include "toolkit_ui.h"

#define TOOLKIT_SCREEN_W    128
#define TOOLKIT_TITLE_BAR_H 13

// Full-width inverted title bar -- same look as universal_card_reader's
// draw_title_bar(), promoted here so every module can share it.
void toolkit_ui_draw_title_bar(Canvas* canvas, const char* title) {
    canvas_draw_box(canvas, 0, 0, TOOLKIT_SCREEN_W, TOOLKIT_TITLE_BAR_H);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    int w = canvas_string_width(canvas, title);
    canvas_draw_str(canvas, (TOOLKIT_SCREEN_W - w) / 2, 10, title);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
}
