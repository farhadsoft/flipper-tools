#include "reader_lf.h"
#include "reader_ui.h"
#include "card_info.h"
#include <lfrfid/protocols/lfrfid_protocols.h>
#undef TAG
#include "../toolkit_app.h"
#undef TAG
#define TAG "UniCardReader"

/*
 * lfrfid_worker_stop() only asks the worker thread to leave read/emulate mode;
 * it does not wait for it. If the NFC side grabs the radio while that teardown
 * is still in flight the firmware wedges hard enough to take USB down with it,
 * so the worker thread is joined here and re-created in
 * reader_start_lf_phase()/reader_start_lf_emulation(). That keeps every
 * transition a strict stop -> release -> start sequence.
 */
void reader_stop_lf(ReaderApp* app) {
    if(app->lf_reading || app->lf_emulating) {
        lfrfid_worker_stop(app->worker);
        app->lf_reading = false;
        app->lf_emulating = false;
    }
    if(app->lf_thread_running) {
        lfrfid_worker_stop_thread(app->worker);
        app->lf_thread_running = false;
    }
}

// Runs on the LF worker thread. Only ReadDone means a decoded card; the
// other results are intermediate progress reports.
static void reader_lf_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    ReaderApp* app = context;
    if(result != LFRFIDWorkerReadDone) return;

    app->lf_protocol = protocol;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(ReaderEventLfRead, app->toolkit->gen));
}

void reader_start_lf_phase(ReaderApp* app) {
    reader_stop_all(app);
    app->toolkit->gen++;
    app->lf_phase = true;
    reader_set_scanning(app, true);
    reader_switch_view(app, ReaderViewScan);

    FURI_LOG_D(TAG, "phase: LF (gen %lu)", (unsigned long)app->toolkit->gen);
    lfrfid_worker_start_thread(app->worker);
    app->lf_thread_running = true;
    lfrfid_worker_read_start(app->worker, LFRFIDWorkerReadTypeAuto, reader_lf_callback, app);
    app->lf_reading = true;
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(LF_PHASE_MS));
}

// GUI thread only. The LF phase owns the worker thread; it is already stopped
// on the info/actions screens, so emulation starts it again (see reader_stop_lf).
void reader_start_lf_emulation(ReaderApp* app) {
    reader_stop_all(app);
    app->toolkit->gen++;
    lfrfid_worker_start_thread(app->worker);
    app->lf_thread_running = true;
    lfrfid_worker_emulate_start(app->worker, (LFRFIDProtocol)app->lf_protocol);
    app->lf_emulating = true;
    const char* name = protocol_dict_get_name(app->dict, app->lf_protocol);
    FURI_LOG_I(TAG, "emulating LF: %s", name ? name : "Unknown");
    reader_enter_emulating(app, name ? name : "LF card");
}

void reader_lf_handle_read(ReaderApp* app) {
    if(!app->lf_phase || !app->lf_reading) return;
    reader_stop_all(app);
    app->toolkit->gen++;
    app->card = ReaderCardLf;

    const char* name = protocol_dict_get_name(app->dict, app->lf_protocol);
    size_t size = protocol_dict_get_data_size(app->dict, app->lf_protocol);
    if(size > ID_MAX_LEN) size = ID_MAX_LEN;
    protocol_dict_get_data(app->dict, app->lf_protocol, app->scratch_id, size);
    app->scratch_id_len = size;

    FuriString* lf_hex = furi_string_alloc();
    reader_cat_hex(lf_hex, app->scratch_id, size);
    FURI_LOG_I(
        TAG, "LF read: %s, ID %s", name ? name : "Unknown", furi_string_get_cstr(lf_hex));
    furi_string_free(lf_hex);

    reader_report_begin(app);
    card_info_format_lf(
        app->info_text, name ? name : "Unknown", app->scratch_id, app->scratch_id_len);
    reader_report_show(app, name ? name : "Unknown");
}
