#pragma once

#include "recorder_app.h"

// Official firmware default list (lib/subghz/subghz_setting.c
// subghz_frequency_list[]), FREQUENCY_FLAG_DEFAULT stripped. Defined here
// (not extern'd from recorder_radio.c) so COUNT_OF() works in every
// translation unit that includes this header without a second, easily
// stale, hand-maintained size constant.
static const uint32_t sub_rec_freqs[] = {
    300000000, 303875000, 304250000, 310000000, 315000000, 318000000,
    390000000, 418000000, 433075000, 433420000, 433920000, 434420000,
    434775000, 438900000, 868350000, 915000000, 925000000,
};
#define SUB_REC_FREQ_DEFAULT_IDX 10 // 433.92 MHz

// Only fork-stable preset ids -- FuriHalSubGhzPreset ids 4..8 drift between
// official and Momentum firmware; never compile in anything above id 3.
typedef struct {
    const char* label; // shown in the UI, and written as preset->name
    FuriHalSubGhzPreset preset; // id <= 3 only
    const char* file_preset; // exact string written to / read from .sub
} SubRecModulation;
static const SubRecModulation sub_rec_mods[] = {
    {"AM650", FuriHalSubGhzPresetOok650Async, "FuriHalSubGhzPresetOok650Async"},
    {"AM270", FuriHalSubGhzPresetOok270Async, "FuriHalSubGhzPresetOok270Async"},
    {"FM238", FuriHalSubGhzPreset2FSKDev238Async, "FuriHalSubGhzPreset2FSKDev238Async"},
};

// Subset of the official RSSI threshold table
// (subghz_scene_receiver_config.c raw_theshold_rssi_value[]). The parallel
// label strings live in subghz_auto_recorder.c: recorder_radio.c never
// displays them, and a static array unused in a translation unit is a
// warning under -Wall.
static const float sub_rec_triggers[] = {-85.0f, -80.0f, -75.0f, -70.0f, -65.0f, -60.0f};
#define SUB_REC_TRIGGER_DEFAULT_IDX 3 // -70 dBm

// Radio session lifecycle -- GUI thread only. See CLAUDE.md crash rule 5:
// every one of these gates on app->state before touching the CC1101 driver.
void sub_rec_radio_alloc(SubRecApp* app);
void sub_rec_radio_free(SubRecApp* app);
void sub_rec_listen_start(SubRecApp* app);
void sub_rec_listen_stop(SubRecApp* app);

// Auto-capture mechanics, called from the RSSI tick handler's decision logic
// in subghz_auto_recorder.c.
void sub_rec_capture_begin(SubRecApp* app);
void sub_rec_capture_end(SubRecApp* app, bool capped);

// Replay (TX) and its two distinct teardowns. sub_rec_tx_stop() and
// sub_rec_tx_abort() are also called directly from sub_rec_app_free() when
// the app exits mid-send or mid-abort -- see CLAUDE.md crash rule 5.
void sub_rec_replay(SubRecApp* app);
void sub_rec_tx_stop(SubRecApp* app);
void sub_rec_tx_abort(SubRecApp* app);
void sub_rec_handle_tx_poll(SubRecApp* app);
