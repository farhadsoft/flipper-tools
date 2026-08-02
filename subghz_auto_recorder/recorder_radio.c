#include "recorder_radio.h"
#include "recorder_ui.h"

#include <stdio.h>
#include <string.h>

#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/subghz_protocol_registry.h>

/* ------------------------- worker/receiver callbacks ------------------ */

// Both run on the SubGhzWorker thread (2048-byte stack) -- keep them tiny.
static void sub_rec_pair(void* context, bool level, uint32_t duration) {
    SubRecApp* app = context;
    subghz_receiver_decode(app->receiver, level, duration);
}

// Overrun means the 4096-entry stream buffer filled while the worker thread
// was not draining it -- the exact failure the capture-open window in
// sub_rec_capture_begin() could cause. Logged so verification 4b can assert
// its absence instead of guessing. app->state is read here purely for the
// message.
static void sub_rec_overrun(void* context) {
    SubRecApp* app = context;
    FURI_LOG_W(TAG, "worker overrun (state=%d)", (int)app->state);
    subghz_receiver_reset(app->receiver);
}

// Runs on the SubGhzWorker thread. SubGhzProtocolTypeDynamic == 2 is
// fork-stable (CLAUDE.md crash rule 3). app->rolling is volatile -- see its
// declaration in recorder_app.h for why that is required and sufficient. It
// is cleared at capture begin and can only be set during the burst, so it is
// consumed at capture end.
static void
    sub_rec_decoded_callback(SubGhzReceiver* receiver, SubGhzProtocolDecoderBase* base, void* context) {
    UNUSED(receiver);
    SubRecApp* app = context;
    if(base->protocol->type == SubGhzProtocolTypeDynamic) {
        app->rolling = true;
    }
}

/* ---------------------------- session lifecycle ------------------------ */

// Call order taken from applications/main/subghz/helpers/subghz_txrx.c:
// subghz_txrx_alloc().
void sub_rec_radio_alloc(SubRecApp* app) {
    app->env = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(app->env, &subghz_protocol_registry);
    // Rainbow-table filenames must be set even though we never load the
    // keystore: the Came Atomo / Nice Flor-S / Alutech decoders dereference
    // them when they fire.
    subghz_environment_set_came_atomo_rainbow_table_file_name(app->env, SUBGHZ_CAME_ATOMO_DIR_NAME);
    subghz_environment_set_alutech_at_4n_rainbow_table_file_name(
        app->env, SUBGHZ_ALUTECH_AT_4N_DIR_NAME);
    subghz_environment_set_nice_flor_s_rainbow_table_file_name(app->env, SUBGHZ_NICE_FLOR_S_DIR_NAME);

    app->receiver = subghz_receiver_alloc_init(app->env);
    // (flag & filter) != 0 -- an OR test, lib/subghz/receiver.c -- so this
    // one call feeds BOTH the RAW decoder (for capture) and every decodable
    // decoder (for the rolling-code warning).
    subghz_receiver_set_filter(app->receiver, SubGhzProtocolFlag_RAW | SubGhzProtocolFlag_Decodable);
    subghz_receiver_set_rx_callback(app->receiver, sub_rec_decoded_callback, app);
    app->raw = (SubGhzProtocolDecoderRAW*)subghz_receiver_search_decoder_base_by_name(
        app->receiver, SUBGHZ_PROTOCOL_RAW_NAME);

    app->worker = subghz_worker_alloc();
    // Context is the APP, not the receiver: both worker callbacks share one
    // context pointer, and the overrun path has to log before it resets.
    subghz_worker_set_overrun_callback(app->worker, sub_rec_overrun);
    subghz_worker_set_pair_callback(app->worker, sub_rec_pair);
    subghz_worker_set_context(app->worker, app);

    // REQUIRED: subghz_file_encoder_worker_start() calls
    // subghz_devices_get_by_name(), which furi_checks the registry.
    subghz_devices_init();
    app->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    // Internal CC1101 only -- no external-module/OTG handling.
    // subghz_devices_begin()/_end() are not called: cc1101_int_interconnect.c
    // has .begin = NULL and .end = furi_hal_subghz_shutdown, and the stock
    // app likewise skips both for the internal radio.
}

// Run after sub_rec_listen_stop(), so the driver is in SubGhzStateIdle --
// subghz_devices_sleep() maps to furi_hal_subghz_sleep(), which furi_checks
// exactly that (CLAUDE.md crash rule 5).
void sub_rec_radio_free(SubRecApp* app) {
    subghz_devices_sleep(app->device);
    subghz_devices_deinit();
    subghz_worker_free(app->worker);
    subghz_receiver_free(app->receiver);
    subghz_environment_free(app->env);
}

// GUI thread only; mirrors subghz_txrx_begin() + subghz_txrx_rx().
void sub_rec_listen_start(SubRecApp* app) {
    // CLAUDE.md crash rule 5: subghz_devices_start_async_rx() furi_checks
    // SubGhzStateIdle, so a duplicate Listen event must not reach it.
    if(app->state != SubRecStateIdle) return;

    app->gen++;

    uint32_t freq = app->custom_freq ? app->custom_freq : sub_rec_freqs[app->freq_idx];
    if(!subghz_devices_is_frequency_valid(app->device, freq)) {
        char l1[24];
        snprintf(
            l1,
            sizeof(l1),
            "%lu.%02lu MHz",
            (unsigned long)(freq / 1000000),
            (unsigned long)(freq / 10000 % 100));
        sub_rec_show_notice(app, "Bad frequency", l1, "", SubRecViewMenu, 0);
        return;
    }

    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, sub_rec_mods[app->mod_idx].preset, NULL);
    subghz_devices_set_frequency(app->device, freq);
    subghz_devices_flush_rx(app->device);
    subghz_devices_start_async_rx(app->device, (void*)subghz_worker_rx_callback, app->worker);
    subghz_worker_start(app->worker);

    furi_string_set_str(app->preset.base.name, sub_rec_mods[app->mod_idx].label);
    app->preset.base.frequency = freq;
    app->preset.base.data = NULL;
    app->preset.base.data_size = 0;

    char line[24];
    sub_rec_format_freq_line(app, line, sizeof(line));
    sub_rec_set_freq_line(app, line, sub_rec_triggers[app->trigger_idx]);

    furi_timer_start(app->rssi_timer, furi_ms_to_ticks(RSSI_POLL_MS));
    sub_rec_set_state(app, SubRecStateArmed, false);
    app->last_above_tick = furi_get_tick();
}

// Shared by sub_rec_capture_end() and sub_rec_listen_stop(). Stop-before-
// close ordering matches the stock firmware (subghz_scene_read_raw.c:
// subghz_txrx_stop() then subghz_protocol_raw_save_to_file_stop()).
static void sub_rec_capture_finish(SubRecApp* app, bool capped, bool restart_worker) {
    app->gen++;
    subghz_worker_stop(app->worker);
    size_t spl = subghz_protocol_raw_get_sample_write(app->raw);
    subghz_protocol_raw_save_to_file_stop(app->raw);

    const char* final_name = "";
    bool kept = spl >= MIN_RAW_SAMPLES;
    if(!kept) {
        storage_simply_remove(app->storage, furi_string_get_cstr(app->capture_path));
        app->dropped++;
    } else {
        // The rolling-code flag is only known now, after the burst has been
        // decoded -- it is the sole persistence of the warning, re-appended
        // by Rename.
        if(app->rolling) {
            FuriString* renamed = furi_string_alloc();
            size_t len = furi_string_size(app->capture_path);
            furi_string_set_n(renamed, app->capture_path, 0, len - 4); // strip ".sub"
            furi_string_cat_str(renamed, "_RC.sub");
            FS_Error err = storage_common_rename(
                app->storage, furi_string_get_cstr(app->capture_path), furi_string_get_cstr(renamed));
            if(err != FSE_OK) {
                FURI_LOG_W(TAG, "rename to _RC failed: %s", storage_error_get_desc(err));
            } else {
                furi_string_set(app->capture_path, renamed);
            }
            furi_string_free(renamed);
        }
        app->saved++;
        const char* full = furi_string_get_cstr(app->capture_path);
        const char* base = strrchr(full, '/');
        final_name = base ? base + 1 : full;
    }

    if(restart_worker) {
        subghz_worker_start(app->worker);
    }
    sub_rec_set_state(app, SubRecStateArmed, capped);
    sub_rec_set_counts(app, app->saved, app->dropped, final_name);
    FURI_LOG_I(TAG, "capture %s: %u samples", kept ? "saved" : "dropped", (unsigned)spl);
}

void sub_rec_capture_begin(SubRecApp* app) {
    app->gen++;

    FuriString* stem_fs = furi_string_alloc();
    sub_rec_next_stem(app, stem_fs);
    char stem[REC_STEM_MAX];
    snprintf(stem, sizeof(stem), "%s", furi_string_get_cstr(stem_fs));
    furi_string_free(stem_fs);

    furi_string_printf(app->capture_path, "%s/%s.sub", REC_DIR, stem);
    char dev_name[REC_STEM_MAX + 16];
    snprintf(dev_name, sizeof(dev_name), "%s/%s", REC_DIR_REL, stem);

    // subghz_worker_stop() furi_thread_joins (lib/subghz/subghz_worker.c),
    // so no feed() can be in flight while the decoder's file state changes.
    // The async RX DMA is deliberately left running: its callback only
    // pushes into a 4096-entry stream buffer and never touches the decoder,
    // so the edges arriving during this ~40 ms open window are replayed into
    // the now-open file when the worker restarts -- nothing is lost.
    subghz_worker_stop(app->worker);

    if(!subghz_protocol_raw_save_to_file_init(app->raw, dev_name, &app->preset.base)) {
        FURI_LOG_E(TAG, "save_to_file_init failed: %s", dev_name);
        subghz_worker_start(app->worker);
        sub_rec_show_notice(app, "Save failed", stem, "", SubRecViewStatus, 0);
        return;
    }

    // Before restarting the worker: the decoded callback runs on the worker
    // thread, so clearing this afterwards could erase a detection made from
    // the burst's first edges.
    app->rolling = false;
    subghz_worker_start(app->worker);

    sub_rec_set_state(app, SubRecStateRecording, false);
    app->capture_start_tick = furi_get_tick();
    app->last_above_tick = app->capture_start_tick;
}

void sub_rec_capture_end(SubRecApp* app, bool capped) {
    sub_rec_capture_finish(app, capped, true);
}

void sub_rec_listen_stop(SubRecApp* app) {
    // Not defensive padding: sub_rec_app_free() calls this on every exit,
    // including exits that never started the radio (CLAUDE.md crash rule 5).
    if(app->state == SubRecStateIdle) return;

    furi_timer_stop(app->rssi_timer);
    if(app->state == SubRecStateRecording) {
        sub_rec_capture_finish(app, false, false);
    }
    if(subghz_worker_is_running(app->worker)) {
        subghz_worker_stop(app->worker);
    }
    subghz_devices_stop_async_rx(app->device);
    subghz_devices_idle(app->device);
    app->gen++;
    sub_rec_set_state(app, SubRecStateIdle, false);
}

/* --------------------------------- replay ------------------------------ */

// Async TX is definitely running -- called from the TxPoll handler on normal
// completion, and from sub_rec_app_free() when the app exits mid-send.
void sub_rec_tx_stop(SubRecApp* app) {
    FURI_LOG_I(TAG, "tx stop (async complete)");
    furi_timer_stop(app->tx_timer);
    subghz_devices_stop_async_tx(app->device);
    subghz_transmitter_stop(app->transmitter);
    subghz_transmitter_free(app->transmitter); // joins the file-encoder thread
    app->transmitter = NULL;
    flipper_format_free(app->fff_tx);
    app->fff_tx = NULL;
    subghz_devices_idle(app->device);
    sub_rec_set_state(app, SubRecStateIdle, false);
    app->gen++;
}

// Async TX never started (or never will), so stop_async_tx() would
// furi_check. Every abort path in sub_rec_replay() runs before anything is
// allocated, so both NULL guards matter; sub_rec_app_free() also calls this
// unconditionally when the app exits without a send in flight.
void sub_rec_tx_abort(SubRecApp* app) {
    if(app->transmitter) {
        subghz_transmitter_free(app->transmitter);
        app->transmitter = NULL;
    }
    if(app->fff_tx) {
        flipper_format_free(app->fff_tx);
        app->fff_tx = NULL;
    }
    subghz_devices_idle(app->device); // no state check; safe from any state
    sub_rec_set_state(app, SubRecStateIdle, false);
    app->gen++;
}

// GUI thread only. Every check that can abort runs before the first
// allocation and before the radio is touched: furi_hal_subghz_tx() (called
// by subghz_devices_set_tx()) drives the CC1101 into CC1101StateTX and waits
// for it, so returning from a half-built replay would leave a keyed
// transmitter with no modulation source, and the rolling-code notice
// re-enters this function from the top, which would leak fff_tx +
// transmitter and call set_tx twice if allocation happened earlier.
void sub_rec_replay(SubRecApp* app) {
    const char* path = furi_string_get_cstr(app->selected_path);

    // This guard is load-bearing: if subghz_file_encoder_worker_start()
    // cannot open the file its thread pushes nothing,
    // subghz_protocol_encoder_raw_yield() returns level_duration_wait()
    // forever, and is_async_complete_tx() never becomes true
    // (lib/subghz/subghz_file_encoder_worker.c).
    if(!storage_file_exists(app->storage, path)) {
        FURI_LOG_W(TAG, "replay: file missing: %s", path);
        sub_rec_show_notice(app, "File missing", "", "", SubRecViewFileMenu, 0);
        return;
    }

    // Nothing is allocated and the radio is untouched here, so the chained
    // re-entry (once rc_warned is set) is clean and idempotent.
    if(!app->rc_warned) {
        const char* base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if(strstr(base, "_RC")) {
            app->rc_warned = true;
            sub_rec_show_notice(
                app,
                "Rolling code?",
                "replay may not",
                "work on the receiver",
                SubRecViewFileMenu,
                SubRecEventFileReplay);
            return;
        }
    }

    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    uint32_t freq = 0;
    FuriString* preset_str = furi_string_alloc();
    bool read_ok = flipper_format_file_open_existing(ff, path) &&
                   flipper_format_read_uint32(ff, "Frequency", &freq, 1) &&
                   flipper_format_read_string(ff, "Preset", preset_str);
    flipper_format_free(ff);
    if(!read_ok) {
        FURI_LOG_W(TAG, "replay: unreadable file: %s", path);
        furi_string_free(preset_str);
        sub_rec_show_notice(app, "Unreadable file", "", "", SubRecViewFileMenu, 0);
        return;
    }

    // Never derive a preset id we did not compile as 0..3 (CLAUDE.md crash
    // rule 1) -- map the string, don't trust the file to name a stable id.
    const SubRecModulation* mod = NULL;
    for(size_t i = 0; i < COUNT_OF(sub_rec_mods); i++) {
        if(furi_string_cmp_str(preset_str, sub_rec_mods[i].file_preset) == 0) {
            mod = &sub_rec_mods[i];
            break;
        }
    }
    if(!mod) {
        char l1[32];
        snprintf(l1, sizeof(l1), "%s", furi_string_get_cstr(preset_str));
        FURI_LOG_W(TAG, "replay: unsupported preset: %s", l1);
        furi_string_free(preset_str);
        sub_rec_show_notice(app, "Unsupported preset", l1, "", SubRecViewFileMenu, 0);
        return;
    }
    furi_string_free(preset_str);

    // CLAUDE.md crash rule 4: subghz_devices_set_frequency() furi_crashes on
    // an invalid frequency.
    if(!subghz_devices_is_frequency_valid(app->device, freq)) {
        FURI_LOG_W(TAG, "replay: bad frequency: %lu", (unsigned long)freq);
        sub_rec_show_notice(app, "Bad frequency", "", "", SubRecViewFileMenu, 0);
        return;
    }

    app->fff_tx = flipper_format_string_alloc();
    subghz_protocol_raw_gen_fff_data(app->fff_tx, path, subghz_devices_get_name(app->device));

    app->transmitter = subghz_transmitter_alloc_init(app->env, SUBGHZ_PROTOCOL_RAW_NAME);
    if(!app->transmitter) {
        sub_rec_tx_abort(app);
        sub_rec_show_notice(app, "TX failed", "encoder alloc", "", SubRecViewFileMenu, 0);
        return;
    }
    if(subghz_transmitter_deserialize(app->transmitter, app->fff_tx) != SubGhzProtocolStatusOk) {
        sub_rec_tx_abort(app);
        sub_rec_show_notice(app, "TX failed", "bad RAW data", "", SubRecViewFileMenu, 0);
        return;
    }

    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, mod->preset, NULL);
    subghz_devices_idle(app->device);
    subghz_devices_set_frequency(app->device, freq);

    // Region gate: set_tx() is furi_hal_subghz_tx(), which returns false
    // when furi_hal_subghz.regulation != SubGhzRegulationTxRx. Must not be
    // worked around.
    if(!subghz_devices_set_tx(app->device)) {
        FURI_LOG_W(TAG, "replay: TX blocked by region lock at %lu Hz", (unsigned long)freq);
        sub_rec_tx_abort(app);
        sub_rec_show_notice(app, "TX blocked", "region locks", "this band", SubRecViewFileMenu, 0);
        return;
    }

    // Second, independent check: furi_hal_subghz_start_async_tx() also
    // returns false on regulation != SubGhzRegulationTxRx, and it also
    // furi_checks state == SubGhzStateIdle -- which holds only because
    // sub_rec_listen_stop() already ran stop_async_rx.
    if(!subghz_devices_start_async_tx(
           app->device, (void*)subghz_transmitter_yield, app->transmitter)) {
        sub_rec_tx_abort(app);
        sub_rec_show_notice(app, "TX refused", "", "", SubRecViewFileMenu, 0);
        return;
    }

    const char* base = strrchr(path, '/');
    FURI_LOG_I(TAG, "replay: TX started: %s", base ? base + 1 : path);
    sub_rec_set_counts(app, app->saved, app->dropped, base ? base + 1 : path);
    sub_rec_set_state(app, SubRecStateSending, false);
    sub_rec_switch_view(app, SubRecViewStatus);
    app->tx_start_tick = furi_get_tick();
    furi_timer_start(app->tx_timer, furi_ms_to_ticks(TX_POLL_MS));
}

// The end-of-file callback hook
// (subghz_protocol_raw_file_encoder_worker_set_callback_end) is deliberately
// not used: it fires repeatedly every 50 ms from the encoder thread while
// the worker winds down, and this poll already covers completion with one
// mechanism and one thread.
void sub_rec_handle_tx_poll(SubRecApp* app) {
    bool done = subghz_devices_is_async_complete_tx(app->device);
    bool timed_out = (furi_get_tick() - app->tx_start_tick) >= furi_ms_to_ticks(TX_TIMEOUT_MS);
    if(!done && !timed_out) return;
    if(timed_out && !done) FURI_LOG_W(TAG, "replay: TX timeout");

    sub_rec_tx_stop(app);
    sub_rec_show_notice(app, done ? "Sent" : "TX timeout", "", "", SubRecViewFileMenu, 0);
}
