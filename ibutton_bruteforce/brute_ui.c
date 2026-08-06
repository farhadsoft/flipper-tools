#include "brute_ui.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include "brute_app.h"

/* Drawing constants for 128x64 monochrome. */
#define BRUTE_UI_BAR_X 4
#define BRUTE_UI_BAR_Y 58
#define BRUTE_UI_BAR_W 120
#define BRUTE_UI_BAR_H 5

static void brute_status_draw_key(Canvas* canvas, const BruteModel* model) {
    char buf[32];
    size_t off = 0;
    for(size_t i = 0; i < model->key_data_len && off < sizeof(buf) - 3; ++i) {
        off += snprintf(buf + off, sizeof(buf) - off, "%02X", model->key_data[i]);
    }
    buf[off] = '\0';
    canvas_draw_str_aligned(canvas, 64, 29, AlignCenter, AlignCenter, buf);
}

static void brute_status_draw_bar(Canvas* canvas, const BruteModel* model) {
    if(model->total == 0) return;

    const uint32_t pct = (model->index * 100) / model->total;
    const uint32_t fill_w = (pct * BRUTE_UI_BAR_W) / 100;

    canvas_draw_frame(canvas, BRUTE_UI_BAR_X, BRUTE_UI_BAR_Y, BRUTE_UI_BAR_W, BRUTE_UI_BAR_H);
    if(fill_w > 0) {
        canvas_draw_box(canvas, BRUTE_UI_BAR_X + 1, BRUTE_UI_BAR_Y + 1, fill_w - 2, BRUTE_UI_BAR_H - 2);
    }
}

static void brute_status_draw_callback(Canvas* canvas, void* _model) {
    const BruteModel* model = _model;
    canvas_clear(canvas);

    if(!model->ethics_accepted) {
        elements_multiline_text_aligned(canvas, 64, 12, AlignCenter, AlignCenter, "Ethics notice");
        elements_multiline_text_aligned(
            canvas,
            64,
            34,
            AlignCenter,
            AlignCenter,
            "Only test readers you\nown or have permission\nto test. Auto-dismisses.");
        return;
    }

    if(model->notice_active) {
        elements_multiline_text_aligned(canvas, 64, 24, AlignCenter, AlignCenter, "Armed");
        elements_multiline_text_aligned(
            canvas, 64, 44, AlignCenter, AlignCenter, "Hold to reader...");
        return;
    }

    char buf[48];

    /* Header line: mode + protocol (protocol only for sequential). */
    if(model->mode == BruteModeSequential) {
        snprintf(
            buf,
            sizeof(buf),
            "%s / %s",
            brute_mode_name(model->mode),
            brute_protocol_item_name(NULL, model->protocol_item));
    } else {
        snprintf(buf, sizeof(buf), "%s", brute_mode_name(model->mode));
    }
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignCenter, buf);

    /* Index / total. */
    snprintf(buf, sizeof(buf), "%lu / %lu", (unsigned long)model->index, (unsigned long)model->total);
    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignCenter, buf);

    /* Current key hex. */
    brute_status_draw_key(canvas, model);

    /* Timing. */
    snprintf(
        buf,
        sizeof(buf),
        "%lu k/m  %lus  ETA %lus",
        (unsigned long)model->keys_per_min,
        (unsigned long)(model->elapsed_ms / 1000),
        (unsigned long)(model->eta_ms / 1000));
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, buf);

    /* State + progress bar. */
    const char* state_str = "Idle";
    switch(model->state) {
    case BruteStateArmed:
        state_str = "Armed";
        break;
    case BruteStatePresent:
        state_str = "Present";
        break;
    case BruteStateGap:
        state_str = "Gap";
        break;
    case BruteStateDone:
        state_str = "Done";
        break;
    default:
        break;
    }
    canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter, state_str);

    brute_status_draw_bar(canvas, model);
}

View* brute_status_view_alloc(BruteApp* app) {
    furi_check(app);
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(BruteModel));
    view_set_context(view, app);
    view_set_draw_callback(view, brute_status_draw_callback);
    return view;
}

void brute_status_view_free(View* view) {
    view_free(view);
}

void brute_status_set_model(
    View* view,
    BruteMode mode,
    BruteState state,
    BruteProtocolItem protocol_item,
    uint32_t index,
    uint32_t total,
    const uint8_t* key_data,
    size_t key_data_len,
    uint32_t keys_per_min,
    uint32_t elapsed_ms,
    uint32_t eta_ms,
    bool notice_active,
    bool ethics_accepted) {
    with_view_model(
        view,
        BruteModel * model,
        {
            model->mode = mode;
            model->state = state;
            model->protocol_item = protocol_item;
            model->index = index;
            model->total = total;
            model->key_data_len = key_data_len < sizeof(model->key_data) ? key_data_len :
                                                                         sizeof(model->key_data);
            memcpy(model->key_data, key_data, model->key_data_len);
            model->keys_per_min = keys_per_min;
            model->elapsed_ms = elapsed_ms;
            model->eta_ms = eta_ms;
            model->notice_active = notice_active;
            model->ethics_accepted = ethics_accepted;
        },
        true);
}

void brute_status_set_index(View* view, uint32_t index) {
    with_view_model(view, BruteModel * model, { model->index = index; }, true);
}

void brute_status_set_state(View* view, BruteState state) {
    with_view_model(view, BruteModel * model, { model->state = state; }, true);
}

void brute_status_set_key(View* view, const uint8_t* data, size_t len) {
    with_view_model(
        view,
        BruteModel * model,
        {
            model->key_data_len = len < sizeof(model->key_data) ? len : sizeof(model->key_data);
            memcpy(model->key_data, data, model->key_data_len);
        },
        true);
}

void brute_status_set_notice(View* view, bool active) {
    with_view_model(view, BruteModel * model, { model->notice_active = active; }, true);
}

void brute_status_set_ethics(View* view, bool accepted) {
    with_view_model(view, BruteModel * model, { model->ethics_accepted = accepted; }, true);
}

void brute_status_set_timing(View* view, uint32_t keys_per_min, uint32_t elapsed_ms, uint32_t eta_ms) {
    with_view_model(
        view,
        BruteModel * model,
        {
            model->keys_per_min = keys_per_min;
            model->elapsed_ms = elapsed_ms;
            model->eta_ms = eta_ms;
        },
        true);
}
