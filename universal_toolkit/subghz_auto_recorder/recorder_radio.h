#pragma once

#include "recorder_app.h"

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

// Frequency-sweep RSSI scan, driven by the same rssi_timer as listen mode
// (see sub_rec_scan_step()). Deliberately never calls
// subghz_devices_start_async_rx()/_stop_async_rx(): the sweep only ever
// calls subghz_devices_set_rx(), which does not move
// furi_hal_subghz.state off SubGhzStateIdle, so stop_async_rx() would
// furi_check.
void sub_rec_scan_start(SubRecApp* app);
void sub_rec_scan_stop(SubRecApp* app);
void sub_rec_scan_step(SubRecApp* app); // one table entry per RSSI tick

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

// Live decoded readout. sub_rec_decoded_callback() (SubGhzWorker thread)
// fills app->live_proto/live_key under app->live_mutex; publish copies them
// into the view model and is called from the RSSI tick on the GUI thread.
void sub_rec_live_publish(SubRecApp* app);
void sub_rec_live_clear(SubRecApp* app);

// Hopper: one hop decision, called from the RSSI tick while Armed. Returns
// true when it moved the radio to a different frequency, which makes this
// tick's RSSI reading stale -- the caller must not start a capture on it.
bool sub_rec_hopper_step(SubRecApp* app);

// RX speaker mirror (stock subghz_txrx_speaker_on/off parity). app->speaker_held
// is the sole owner of the matching furi_hal_speaker_release().
void sub_rec_speaker_on(SubRecApp* app);
void sub_rec_speaker_off(SubRecApp* app);
// Notification blip. Parks the speaker first when this app owns it: the
// notification service cannot play a sound on a speaker it does not own.
void sub_rec_alert(SubRecApp* app, const NotificationSequence* seq);

// Add manually: build and save a .sub for `proto` from an explicit bit count
// and 8-byte key, using the firmware's own serializer (stock
// subghz_txrx_gen_data_protocol()'s serialize-then-update trick), and select
// it as the current file. Raises its own notice on every failure path.
void sub_rec_add_manual(SubRecApp* app, const char* proto, uint8_t bits, const uint8_t* key);

// Opt-in KeeLoq manufacture-key database load; see its definition for why it
// is gated and why it runs after the config load, not in sub_rec_radio_alloc().
void sub_rec_keystore_load(SubRecApp* app);
