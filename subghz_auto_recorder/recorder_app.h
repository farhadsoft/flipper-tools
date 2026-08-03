#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/number_input.h>
#include <gui/modules/text_input.h>
#include <storage/storage.h>

#include <lib/subghz/types.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/protocols/raw.h>
#include <lib/flipper_format/flipper_format.h>

#define TAG "SubGhzAutoRec"

#define REC_DIR     EXT_PATH("subghz/auto_rec")
#define REC_DIR_REL "auto_rec" // relative to SUBGHZ_RAW_FOLDER; see recorder_radio.c
#define REC_STEM_MAX 48
// Longest path this app builds: REC_DIR "/" <stem> "_RC.sub". EXT_PATH() is a
// string-literal concat, so both sizeof()s are exact and already count a NUL.
#define REC_PATH_MAX (sizeof(REC_DIR) + REC_STEM_MAX + sizeof("_RC.sub"))
// Longest text a status/notice line can hold. Every scratch buffer that feeds
// one is sized from this instead of an arbitrary number -- SubRecModel
// truncates anything longer anyway.
#define REC_TEXT_LINE_MAX 32

#define RSSI_POLL_MS      25
#define RSSI_REDRAW_EVERY 5 // repaint every 5th poll (~8 Hz); see recorder_ui.c
#define CAPTURE_HANG_MS   500 // sub-threshold time that ends a capture
#define CAPTURE_MAX_MS    10000 // hard cap on one capture; see cooldown in subghz_auto_recorder.c
#define MIN_RAW_SAMPLES   40 // fewer edges than this -> discard the file
#define TX_POLL_MS        50
#define TX_TIMEOUT_MS     15000 // FEWorker waits forever if its file is unreadable
#define NOTICE_MS         1600
#define RSSI_FLOOR_DBM    (-95.0f) // left edge of the on-screen bar
#define RSSI_CEIL_DBM     (-35.0f) // right edge

// Generation-stamped events -- identical scheme to universal_card_reader
// (reader_app.h invariant 2). Keep every SubRecCustomEvent value below 256.
#define EVENT_ID(e)         ((e) & 0xFFu)
#define EVENT_GEN(e)        ((e) >> 8u)
#define EVENT_MAKE(id, gen) ((uint32_t)(id) | ((uint32_t)(gen) << 8u))

typedef enum {
    SubRecStateIdle, // menu / settings, radio down
    SubRecStateArmed, // RX up, no capture file open
    SubRecStateRecording, // RX up, RAW file open
    SubRecStateSending, // async TX in progress
} SubRecState;
// There is deliberately no SubRecStateNotice: a notice is an overlay flag
// (app->notice_active), not a state. Making it a state would clobber the
// radio/logic state every time a message appeared during listening.

typedef enum {
    SubRecEventRssiTick = 1,
    SubRecEventTxPoll,
    SubRecEventNoticeDone,
    SubRecEventMenuListen,
    SubRecEventMenuSettings,
    SubRecEventMenuSaved,
    SubRecEventMenuExit,
    SubRecEventFileReplay,
    SubRecEventFileDelete,
    SubRecEventFileRename,
    SubRecEventFileBack,
} SubRecCustomEvent;

typedef enum {
    SubRecViewStatus, // custom View: listening / sending / notice
    SubRecViewMenu, // Submenu: main menu
    SubRecViewSettings, // VariableItemList
    SubRecViewFileMenu, // Submenu: per-file actions
    SubRecViewNumber, // NumberInput: custom frequency in kHz
    SubRecViewText, // TextInput: rename
} SubRecView;

// Momentum's SubGhzRadioPreset appends float latitude/longitude past the
// official four members (lib/subghz/types.h). The first four members match
// the SDK layout exactly; the tail is headroom so a fork that reads it gets
// zeros instead of our neighbouring stack. memset the whole thing once at
// alloc and always pass &app->preset.base to firmware calls.
typedef struct {
    SubGhzRadioPreset base;
    float fork_tail[4];
} SubRecPreset;

typedef struct {
    SubRecState state;
    bool cooldown;
    bool notice_active;
    float rssi;
    float trigger;
    size_t samples;
    uint32_t saved;
    uint32_t dropped;
    char freq_line[24];
    char last_file[REC_TEXT_LINE_MAX];
    char notice_title[24];
    char notice_l1[REC_TEXT_LINE_MAX];
    char notice_l2[REC_TEXT_LINE_MAX];
} SubRecModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view; // status view (SubRecViewStatus)
    Submenu* menu; // main menu
    Submenu* file_menu; // per-file actions
    VariableItemList* settings;
    // The Frequency row, captured so the settings enter callback (which only
    // receives a list-position index, not a value-index) can check whether
    // the row is currently showing the "Custom" slot. Owned by the app
    // struct rather than a file-static so all app state lives in one place.
    VariableItem* freq_item;
    NumberInput* number;
    TextInput* text;
    FuriTimer* rssi_timer; // periodic
    FuriTimer* tx_timer; // periodic
    FuriTimer* notice_timer; // one-shot
    volatile uint32_t gen;
    SubRecView current_view;
    SubRecState state;
    Storage* storage;

    // Radio handles -- owned and manipulated by recorder_radio.c.
    const SubGhzDevice* device;
    SubGhzEnvironment* env;
    SubGhzReceiver* receiver;
    SubGhzWorker* worker;
    SubGhzProtocolDecoderRAW* raw;
    SubGhzTransmitter* transmitter; // NULL whenever no transmit is in flight
    FlipperFormat* fff_tx; // NULL whenever no transmit is in flight

    SubRecPreset preset; // preset.base.name is a FuriString, alloc'd once, freed at app free
    uint8_t freq_idx;
    uint8_t mod_idx;
    uint8_t trigger_idx;
    uint32_t custom_freq; // 0 = use the table

    uint32_t saved;
    uint32_t dropped;
    FuriString* capture_path;
    FuriString* selected_path;
    char rename_buf[REC_STEM_MAX];

    bool ethics_shown;
    // Written on the SubGhzWorker thread (sub_rec_decoded_callback), read on
    // the GUI thread (sub_rec_capture_end). volatile is required and is
    // sufficient: single word, single writer, no ordering dependency on any
    // other field.
    volatile bool rolling;

    bool cooldown;
    bool last_above;
    SubRecState last_state;
    uint32_t tick_count; // redraw decimation
    bool rc_warned; // per selected file; cleared on (re)pick and on delete

    uint32_t capture_start_tick;
    uint32_t last_above_tick;
    uint32_t tx_start_tick;

    // Notice overlay trio -- see sub_rec_show_notice().
    bool notice_active;
    SubRecView notice_return_view;
    SubRecCustomEvent notice_next; // 0 = just switch back to notice_return_view
} SubRecApp;

// Core services implemented in subghz_auto_recorder.c.
void sub_rec_switch_view(SubRecApp* app, SubRecView view);
void sub_rec_show_notice(
    SubRecApp* app,
    const char* title,
    const char* l1,
    const char* l2,
    SubRecView return_view,
    SubRecCustomEvent next_event);
void sub_rec_next_stem(SubRecApp* app, FuriString* out);
// Formats "<freq> MHz  <mod label>" for the current frequency (custom if
// set, else the table) and modulation -- shared so the listening status
// line and the Trigger settings callback (which must recompute it without
// reading the view model; recorder_ui.c is the only file allowed to do
// that) never drift apart.
void sub_rec_format_freq_line(SubRecApp* app, char* out, size_t out_size);
