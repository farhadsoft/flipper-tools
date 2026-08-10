#include "gpio_info.h"
#include "../../toolkit_log.h"
#include "../../toolkit_ui.h"

#include <gui/view.h>
#include <furi_hal_gpio.h>
#include <furi_hal_power.h>
#include <furi_hal_rtc.h>

// The 8 pins on the Flipper Zero's external GPIO header -- confirmed
// present and linkable in api_symbols.csv (STEP 0). Order matches the
// physical header, top to bottom.
static const GpioPin* const gpio_info_pins[] = {
    &gpio_ext_pc0,
    &gpio_ext_pc1,
    &gpio_ext_pc3,
    &gpio_ext_pb2,
    &gpio_ext_pb3,
    &gpio_ext_pa4,
    &gpio_ext_pa6,
    &gpio_ext_pa7,
};
static const char* const gpio_info_pin_names[] = {
    "PC0",
    "PC1",
    "PC3",
    "PB2",
    "PB3",
    "PA4",
    "PA6",
    "PA7",
};
#define GPIO_INFO_PIN_COUNT (sizeof(gpio_info_pins) / sizeof(gpio_info_pins[0]))

#define GPIO_INFO_REFRESH_EVENT 0u
#define GPIO_INFO_REFRESH_MS    200

typedef struct {
    View* view;
    FuriTimer* refresh_timer;
} GpioInfoCtx;

#define GPIO_INFO_FLASH_TICKS 3 // ~600 ms at the 200 ms refresh tick
typedef struct {
    bool level[GPIO_INFO_PIN_COUNT];
    uint8_t flash[GPIO_INFO_PIN_COUNT]; // >0 = cell drawn inverted; decrements each tick
    uint8_t phase; // status-bar animation phase
    uint8_t battery; // furi_hal_power_get_pct()
} GpioInfoModel;

/* --------------------------------- draw ------------------------------------ */

#define GPIO_GRID_ROW_Y0  20 // row baselines: 20, 28, 36, 44 (8 px pitch)
#define GPIO_GRID_CELL_H   8
#define GPIO_GRID_COL0_X   6 // left column label x
#define GPIO_GRID_COL1_X  70 // right column label x
#define GPIO_GRID_DISC_DX 26 // disc center = label x + 26
#define GPIO_GRID_DIV_X   64 // column divider x

static void gpio_info_draw_callback(Canvas* canvas, void* model) {
    GpioInfoModel* m = model;
    canvas_clear(canvas);
    ui_status_bar(canvas, "GPIO", NULL, UiStatusLive, m->battery, m->phase);

    canvas_draw_line(canvas, GPIO_GRID_DIV_X, UI_RULE_Y + 2, GPIO_GRID_DIV_X, UI_FOOTER_Y - 1);

    for(size_t i = 0; i < GPIO_INFO_PIN_COUNT; i++) {
        int col = (int)(i % 2);
        int row = (int)(i / 2);
        int x = col ? GPIO_GRID_COL1_X : GPIO_GRID_COL0_X;
        int y = GPIO_GRID_ROW_Y0 + row * GPIO_GRID_CELL_H;

        if(m->flash[i] > 0) {
            canvas_draw_box(canvas, x - 2, y - 6, 36, 8);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_draw_str(canvas, x, y, gpio_info_pin_names[i]);
        if(m->level[i]) {
            canvas_draw_disc(canvas, x + GPIO_GRID_DISC_DX, y - 3, 2);
        } else {
            canvas_draw_circle(canvas, x + GPIO_GRID_DISC_DX, y - 3, 2);
        }

        if(m->flash[i] > 0) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    ui_draw_centered(canvas, UI_FOOTER_Y, "Back: exit module");
}

/* -------------------------------- timer ------------------------------------ */

// Runs on the TimersSrv thread: posts only, never touches GPIO or the view
// model directly -- pin reads and with_view_model both happen in
// gpio_info_event() on the GUI thread.
static void gpio_info_timer_callback(void* context) {
    ToolkitApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(GPIO_INFO_REFRESH_EVENT, app->gen));
}

/* ------------------------------- lifecycle ---------------------------------- */

void gpio_info_enter(ToolkitApp* app) {
    GpioInfoCtx* ctx = malloc(sizeof(GpioInfoCtx));
    app->active_ctx = ctx;

    ctx->view = view_alloc();
    view_allocate_model(ctx->view, ViewModelTypeLocking, sizeof(GpioInfoModel));
    view_set_draw_callback(ctx->view, gpio_info_draw_callback);
    view_dispatcher_add_view(app->view_dispatcher, TOOLKIT_VIEW_BASE_GPIO, ctx->view);

    // Acquire: every header pin as a plain digital input, no pull. This
    // module only reads, never drives, so pull choice cannot affect
    // anything else wired to the header.
    for(size_t i = 0; i < GPIO_INFO_PIN_COUNT; i++) {
        furi_hal_gpio_init(gpio_info_pins[i], GpioModeInput, GpioPullNo, GpioSpeedLow);
    }

    ctx->refresh_timer = furi_timer_alloc(gpio_info_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(ctx->refresh_timer, furi_ms_to_ticks(GPIO_INFO_REFRESH_MS));

    view_dispatcher_switch_to_view(app->view_dispatcher, TOOLKIT_VIEW_BASE_GPIO);

    ToolkitLogRecord rec = {.ts = furi_hal_rtc_get_timestamp(), .subsys = ToolkitSubsysGpio};
    snprintf(rec.summary, sizeof(rec.summary), "gpio module opened");
    rec.file[0] = '\0';
    toolkit_log_append(app, &rec);
}

void gpio_info_exit(ToolkitApp* app) {
    GpioInfoCtx* ctx = app->active_ctx;

    furi_timer_stop(ctx->refresh_timer);
    furi_timer_free(ctx->refresh_timer);

    // Release: back to the firmware's own idle state for an unused header
    // pin (analog, no pull) so nothing about this module's use is
    // observable once it exits.
    for(size_t i = 0; i < GPIO_INFO_PIN_COUNT; i++) {
        furi_hal_gpio_init(gpio_info_pins[i], GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    }

    view_dispatcher_remove_view(app->view_dispatcher, TOOLKIT_VIEW_BASE_GPIO);
    view_free(ctx->view);
    free(ctx);
    app->active_ctx = NULL;
}

bool gpio_info_event(ToolkitApp* app, uint32_t id) {
    if(id != GPIO_INFO_REFRESH_EVENT) return false;
    GpioInfoCtx* ctx = app->active_ctx;

    bool level[GPIO_INFO_PIN_COUNT];
    for(size_t i = 0; i < GPIO_INFO_PIN_COUNT; i++) {
        level[i] = furi_hal_gpio_read(gpio_info_pins[i]);
    }

    with_view_model(
        ctx->view,
        GpioInfoModel * m,
        {
            for(size_t i = 0; i < GPIO_INFO_PIN_COUNT; i++) {
                if(level[i] != m->level[i]) m->flash[i] = GPIO_INFO_FLASH_TICKS;
                else if(m->flash[i] > 0) m->flash[i]--;
            }
            memcpy(m->level, level, sizeof(level));
            m->phase++;
            m->battery = furi_hal_power_get_pct();
        },
        true);
    return true;
}

bool gpio_info_nav(ToolkitApp* app) {
    toolkit_exit_module(app);
    return true;
}
