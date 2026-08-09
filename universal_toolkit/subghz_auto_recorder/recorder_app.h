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
// storage_simply_mkdir() creates ONE level, so both are needed (same reason
// sub_rec_ensure_dir() exists for REC_DIR).
#define REC_CONF_ROOT    EXT_PATH("apps_data")
#define REC_CONF_DIR     EXT_PATH("apps_data/subghz_auto_recorder")
#define REC_CONF_PATH    REC_CONF_DIR "/settings.conf"
#define REC_CONF_TYPE    "SubGhz Auto Recorder settings"
#define REC_CONF_VERSION 1
#define REC_STEM_MAX 48
// Longest path this app builds: REC_DIR "/" <stem> "_RC_D.sub" (the decoded
// sidecar of a rolling-code capture). EXT_PATH() is a string-literal concat, so
// both sizeof()s are exact and already count a NUL.
#define REC_PATH_MAX (sizeof(REC_DIR) + REC_STEM_MAX + sizeof("_RC_D.sub"))
// Longest text a status/notice line can hold. Every scratch buffer that feeds
// one is sized from this instead of an arbitrary number -- SubRecModel
// truncates anything longer anyway.
#define REC_TEXT_LINE_MAX 32

// Longest capture label (the "Note:" key appended inside a .sub; see A1 in
// the feature plan). Independent of REC_TEXT_LINE_MAX: a label is content,
// not a status/notice line.
#define REC_NOTE_MAX 32
#define REC_PROFILE_MAX      8
#define REC_PROFILE_NAME_MAX 16 // spaces are the parse delimiter; see sub_rec_profile_save_result()
#define REC_DUP_MAX 32 // dedup ring depth; ~1 uint32 comparison per entry per capture
// sub_rec_build_settings() row order: Frequency=0, Modulation=1, Trigger=2,
// Max captures=3, Max minutes=4, Dedup=5, Profiles=6 (D1/D2 inserted rows
// before it).
#define REC_SETTINGS_ROW_PROFILES 6
// storage_dir_read() truncates into a too-small buffer, and a truncated name
// builds a path that does not exist -- storage_simply_remove() returns true
// for an already-absent item (storage.h), so a truncated name would be
// counted as deleted while the real file stayed. 255 is the FatFS long-name
// cap, +1 for the NUL, so truncation cannot happen.
#define REC_NAME_MAX 256
// Clear all re-enumerates until a pass deletes nothing. Deleting the entry
// f_readdir just returned does not skip entries on FAT (the slot is marked
// free, nothing is relocated), but the re-enumeration means this app does not
// depend on that. The cap only guards a storage layer that reports a
// successful remove for a file that stays enumerable, which would otherwise
// spin the GUI thread forever.
#define REC_CLEAR_MAX_PASSES 8

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
// Settle time between switching the CC1101 to RX on a new frequency and
// reading RSSI. Taken from the stock frequency analyzer worker
// (applications/main/subghz/helpers/subghz_frequency_analyzer_worker.c:
// furi_delay_ms(2) after cc1101_switch_to_rx, in both its coarse and fine
// sweeps). Runs on the GUI thread, once per RSSI_POLL_MS tick.
//
// That stock figure was tuned under the analyzer worker's own flat-response
// AGC override (AGCCTRL0..2, MDMCFG3/4) -- registers no FAP can write, since
// api_symbols.csv exports zero cc1101_* functions. This app settles under
// the selected preset's AGC instead, so 2 ms is inherited from the stock
// worker, not validated under this app's own AGC.
//
// Tuning ladder if a delta-gate measurement ever shows it is too short:
// 2 -> 4 -> 8 ms, never below 2 (the stock figure; below it the CC1101 may
// not have settled at all) and never above 15 (the tick is RSSI_POLL_MS
// 25 ms and the handler also runs idle/set_frequency/flush_rx/set_rx over
// SPI plus the wrap repaint; 15 ms still leaves ~10 ms of headroom).
//
// Raising this does not lengthen the sweep: rssi_timer is periodic at
// RSSI_POLL_MS regardless of handler runtime, so the extra delay is
// absorbed inside the tick, not added on top of it. Measured at 2 ms:
// sweeps ran 422-430 ms, mean 425 ms -- exactly 17 * 25 ms
// (17 == COUNT_OF(sub_rec_freqs)).
#define SCAN_SETTLE_MS 2

// Analyze view. The waveform is downsampled to one level per screen column at
// parse time, so drawing is O(WAVE_COLS) and the model holds a fixed buffer
// however long the capture is -- a 100 ms and a 10 s capture cost the same.
#define WAVE_COLS     120 // waveform columns; x = 4..123
#define ANA_MOD_MAX   24  // "AM650", or an unrecognised Preset string, truncated
#define ANA_PROTO_MAX 16
#define ANA_KEY_MAX 17 // 8 key bytes as hex + NUL
#define REC_ZOOM_ALL   4 // FIT=0, x2=1, x4=2, x8=3, ALL=4
#define REC_ZOOM_STEPS 5

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
    SubRecStateScanning, // sweeping the table, plain RX, no worker
    SubRecStateAnalyzing, // inspecting a saved capture; no radio, no writes
    SubRecStateStats, // folder + session summary; no radio, no writes
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
    SubRecEventSavedBrowse,
    SubRecEventSavedClearAll,
    SubRecEventSavedBack,
    SubRecEventConfirmYes,
    SubRecEventConfirmNo,
    SubRecEventMenuScan,
    SubRecEventScanLock,
    SubRecEventFileAnalyze,
    SubRecEventAnalyzePage,
    SubRecEventFileLabel,
    SubRecEventSavedClearRaw,
    SubRecEventSavedClearDecoded,
    SubRecEventSavedClearRc,
    SubRecEventSavedStats,
    SubRecEventProfileSave,
    SubRecEventProfilePick,
    SubRecEventAnalyzePanL,
    SubRecEventAnalyzePanR,
    SubRecEventAnalyzeZoom,
    SubRecEventProfileSlot0, // rows use +slot as their submenu index; never dispatched
} SubRecCustomEvent;

typedef enum {
    SubRecViewStatus, // custom View: listening / sending / notice
    SubRecViewMenu, // Submenu: main menu
    SubRecViewSettings, // VariableItemList
    SubRecViewFileMenu, // Submenu: per-file actions
    SubRecViewNumber, // NumberInput: custom frequency in kHz
    SubRecViewText, // TextInput: rename
    SubRecViewSaved, // Submenu: Saved-signals actions (browse / clear all)
    SubRecViewConfirm, // Submenu: destructive-action confirmation
    SubRecViewProfiles, // Submenu: saved profiles
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

// Official firmware default list (lib/subghz/subghz_setting.c
// subghz_frequency_list[]), FREQUENCY_FLAG_DEFAULT stripped. Defined here
// (not extern'd from recorder_radio.c) so COUNT_OF() works in every
// translation unit that includes this header without a second, easily
// stale, hand-maintained size constant -- and it lives here, in
// recorder_app.h, because SubRecModel below sizes scan_dbm[] from
// COUNT_OF(sub_rec_freqs), and recorder_ui.c includes only recorder_app.h,
// not recorder_radio.h. Header-defined means one const copy per
// including translation unit (three, ~204 bytes of flash total) -- accepted
// deliberately over an extern plus a hand-maintained element count.
static const uint32_t sub_rec_freqs[] = {
    300000000, 303875000, 304250000, 310000000, 315000000, 318000000,
    390000000, 418000000, 433075000, 433420000, 433920000, 434420000,
    434775000, 438900000, 868350000, 915000000, 925000000,
};
#define SUB_REC_FREQ_DEFAULT_IDX 10 // 433.92 MHz

// One saved capture's parsed header plus its downsampled waveform. Filled by
// sub_rec_analyze_load() on the GUI thread and handed to the model in a single
// setter call, so a half-parsed capture is never drawn.
typedef struct {
    uint32_t freq; // Hz, from the file's Frequency field
    uint32_t samples; // total RAW_Data values
    uint32_t bytes; // file size
    uint32_t total_us; // sum of |duration|
    uint16_t wave_len; // columns filled; 0 == nothing to draw
    char mod[ANA_MOD_MAX]; // short label, else the raw Preset string
    char proto[ANA_PROTO_MAX]; // Protocol field; "RAW" for every capture today
    uint32_t bit; // decoded bit count; 0 == not a decoded file, nothing to draw
    char key[ANA_KEY_MAX]; // Key as hex, only the bytes `bit` covers
    uint8_t wave[WAVE_COLS]; // 0/1 level per column
    char note[REC_NOTE_MAX]; // "" == no Note key
    uint32_t win_start_us; // window shown on the waveform page
    uint32_t win_us;
    uint8_t zoom; // 0=FIT 1=x2 2=x4 3=x8 4=ALL
} SubRecAnalysis;

// Filled by sub_rec_collect_stats() on the GUI thread in one directory pass.
typedef struct {
    uint32_t files; // .sub files directly in REC_DIR
    uint32_t raw; // captures (not _D)
    uint32_t decoded; // _D sidecars
    uint32_t rc; // names containing _RC
    uint32_t kib; // total size of those files, KiB, rounded down
    uint32_t saved; // session counters, copied when the screen opens
    uint32_t dropped;
    uint32_t dup;
} SubRecStats;

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
    char proto_line[REC_TEXT_LINE_MAX]; // last capture's decoded protocol; "" = none
    char notice_title[24];
    char notice_l1[REC_TEXT_LINE_MAX];
    char notice_l2[REC_TEXT_LINE_MAX];
    int8_t scan_dbm[COUNT_OF(sub_rec_freqs)]; // per-frequency RSSI, floor-filled at scan start
    uint8_t scan_peak; // index of the strongest entry seen; also the OK-lock target
    uint8_t scan_idx; // current frequency index being scanned (0..16)
    SubRecAnalysis ana;
    uint8_t ana_page; // 0 = info, 1 = waveform
    uint8_t anim_phase; // bumps on every redraw tick; drives REC blink, meter pulse, scan sweep
    uint32_t dup; // session duplicate count; 0 = hide the counter (see draw_listening/draw_stats)
    SubRecStats stats;
} SubRecModel;

// A named freq+mod+trigger bundle -- see B2 in the feature plan. Also the
// delivery of "favourite frequencies": loading a profile is a superset of
// jumping to a saved frequency, so no separate favourites list exists.
typedef struct {
    char name[REC_PROFILE_NAME_MAX];
    uint32_t freq; // Hz
    uint8_t mod_idx; // index into sub_rec_mods[]
    uint8_t trigger_idx; // index into sub_rec_triggers[]
} SubRecProfile;

// Forward declaration only -- this header must not depend on
// universal_toolkit/toolkit_app.h; the .c files that dereference
// app->toolkit include it themselves. Full typedef (not just a bare
// `struct ToolkitApp;` tag) so `ToolkitApp*` below is a valid type name
// regardless of include order -- identical redeclaration once the .c file's
// own toolkit_app.h include lands is legal C11/GNU11.
typedef struct ToolkitApp ToolkitApp;

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* view; // status view (SubRecViewStatus)
    Submenu* menu; // main menu
    Submenu* file_menu; // per-file actions
    Submenu* saved_menu; // saved-signals actions
    Submenu* confirm_menu; // Clear all confirmation
    Submenu* profiles_menu; // saved profiles
    VariableItemList* settings;
    // The Frequency row, captured so the settings enter callback (which only
    // receives a list-position index, not a value-index) can check whether
    // the row is currently showing the "Custom" slot. Owned by the app
    // struct rather than a file-static so all app state lives in one place.
    VariableItem* freq_item;
    // Captured the same way freq_item is, so sub_rec_profile_apply() can
    // re-sync both rows by hand (set_current_value_index() never fires the
    // change callback).
    VariableItem* mod_item;
    VariableItem* trigger_item;
    NumberInput* number;
    TextInput* text;
    FuriTimer* rssi_timer; // periodic
    FuriTimer* tx_timer; // periodic
    FuriTimer* notice_timer; // one-shot
    // Set by the module wrapper (universal_toolkit/modules/subghz_rec.c)
    // when running inside the toolkit; NULL standalone. gen now lives on
    // ToolkitApp -- see app->toolkit->gen below.
    ToolkitApp* toolkit;
    bool module_mode; // true when running as a toolkit module
    uint32_t view_base; // this instance's view-id namespace base (set in alloc)
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
    uint8_t scan_idx; // next table entry to measure
    uint8_t scan_peak; // best entry in the sweep in progress
    int8_t scan_peak_dbm; // its RSSI; reset to the floor at each wrap
    uint8_t ana_page; // mirrors the model's copy; sole writer is sub_rec_set_analyze_page()
    // Analyze window state -- app's own mirror (see freq_item's comment for
    // why: nothing outside recorder_ui.c reads the model). total/fit are
    // fixed per loaded file; win_start/win/zoom track the current view and
    // are the only ones sub_rec_analyze_rewindow() touches per keypress.
    uint32_t ana_total_us;
    uint32_t ana_fit_start_us;
    uint32_t ana_fit_us;
    uint32_t ana_win_start_us;
    uint32_t ana_win_us;
    uint8_t ana_zoom;

    uint32_t saved;
    uint32_t dropped;
    FuriString* capture_path;
    FuriString* selected_path;
    char rename_buf[REC_STEM_MAX];
    char note_buf[REC_NOTE_MAX]; // selected file's Note; the TextInput edits this in place
    char profile_name_buf[REC_PROFILE_NAME_MAX]; // TextInput target for Save current...

    bool ethics_shown;
    // Written on the SubGhzWorker thread (sub_rec_decoded_callback), read on
    // the GUI thread (sub_rec_capture_end). volatile is required and is
    // sufficient: single word, single writer, no ordering dependency on any
    // other field.
    volatile bool rolling;
    // Written on the SubGhzWorker thread (sub_rec_decoded_callback), read on the
    // GUI thread in sub_rec_capture_finish() -- but only AFTER its
    // subghz_worker_stop(), which furi_thread_joins the worker, so the join is
    // the synchronisation and volatile only stops the compiler caching it.
    // The decoder instance itself is owned by app->receiver and outlives every
    // capture (subghz_receiver_alloc_init/-_free are the only alloc/free), so the
    // pointer never dangles. Last decoder to fire wins when several match one
    // burst -- same arbitrary-but-harmless rule app->rolling already uses.
    SubGhzProtocolDecoderBase* volatile decoded;

    bool cooldown;
    bool last_above;
    SubRecState last_state;
    uint32_t tick_count; // redraw decimation
    bool rc_warned; // per selected file; cleared on (re)pick and on delete
    uint8_t clear_kind; // index into sub_rec_clear_kinds[], set when the confirm opens
    SubRecProfile profiles[REC_PROFILE_MAX];
    uint8_t profile_n;
    uint8_t profile_sel; // slot the row callback stashed; consumed by sub_rec_handle_profile_pick()
    bool profile_del; // true = long press (delete); false = short press (load)

    // Auto-record session limits (D1). Values, not indices, persist in
    // config -- see sub_rec_max_caps[]/sub_rec_max_mins[].
    uint8_t max_cap_idx;
    uint8_t max_min_idx;
    uint32_t listen_start_tick;
    uint32_t limit_base; // app->saved at listen start; caps count THIS session

    // Session-scoped decode-string dedup ring (D2), off by default.
    bool dedup;
    uint32_t dup;
    uint32_t dup_hash[REC_DUP_MAX];
    uint32_t dup_n;

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
// Non-static: the module wrapper (universal_toolkit/modules/subghz_rec.c)
// calls all four from a different translation unit.
SubRecApp* sub_rec_app_alloc(ViewDispatcher* view_dispatcher);
void sub_rec_app_free(SubRecApp* app);
bool sub_rec_custom_event_callback(void* context, uint32_t event);
bool sub_rec_navigation_callback(void* context);
