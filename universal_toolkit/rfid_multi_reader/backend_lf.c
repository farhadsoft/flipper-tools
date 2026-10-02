/*
 * 125 kHz LF RFID backend.
 *
 * Worker lifecycle ported from universal_card_reader/reader_lf.c. The report
 * is the capability that app's card_info_format_lf() lacks: decoded
 * per-protocol fields via protocol_dict_render_data(), not just raw hex.
 */
#include "backend_lf.h"
#include "rfid_app.h"

#include <lfrfid/lfrfid_worker.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>

typedef struct {
    ProtocolDict* dict;
    LFRFIDWorker* worker;
    uint8_t* id_buf; // protocol_dict_get_max_data_size() bytes
    size_t id_buf_size;
    size_t id_len;
    ProtocolId protocol;
    bool reading;
    bool thread_running;
    RfidDetectCb detect_cb;
    void* detect_ctx;
    RfidReadCb read_cb;
    void* read_ctx;
} LfImpl;

static bool lf_available(void) {
    return true;
}

static void lf_alloc(RfidBackend* self) {
    LfImpl* impl = self->impl;
    // Passing our compile-time LFRFIDProtocolMax against the firmware's own
    // lfrfid_protocols[] is safe when a fork has more entries (Momentum has
    // 26 vs 24, extra ones are simply never decoded); ids always round-trip
    // through the firmware's own array, so names stay correct.
    impl->dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    impl->worker = lfrfid_worker_alloc(impl->dict);
    // protocol_dict_get_data() furi_checks data_size >= protocol_data_size,
    // so a fixed-size buffer would crash on any protocol with a larger
    // payload; size it from the dict's own reported maximum instead.
    impl->id_buf_size = protocol_dict_get_max_data_size(impl->dict);
    impl->id_buf = malloc(impl->id_buf_size);
    impl->protocol = PROTOCOL_NO;
}

static void lf_release(RfidBackend* self) {
    LfImpl* impl = self->impl;
    // The worker thread is owned by the scan, never by the app lifetime;
    // scan_stop() already joined it before release() is ever reached.
    free(impl->id_buf);
    lfrfid_worker_free(impl->worker);
    protocol_dict_free(impl->dict);
}

// Runs on the LF worker thread. Only ReadDone means a decoded card; the
// other results are intermediate progress reports.
static void lf_worker_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    LfImpl* impl = context;
    if(result != LFRFIDWorkerReadDone) return;
    impl->protocol = protocol;
    impl->detect_cb(impl->detect_ctx);
}

static void lf_scan_start(RfidBackend* self, RfidDetectCb cb, void* ctx) {
    LfImpl* impl = self->impl;
    impl->detect_cb = cb;
    impl->detect_ctx = ctx;
    impl->protocol = PROTOCOL_NO;
    impl->id_len = 0;
    lfrfid_worker_start_thread(impl->worker);
    impl->thread_running = true;
    lfrfid_worker_read_start(impl->worker, LFRFIDWorkerReadTypeAuto, lf_worker_callback, impl);
    impl->reading = true;
}

// lfrfid_worker_stop() only asks the worker to leave read mode; it does not
// wait. lfrfid_worker_stop_thread() joins. Without the join, starting the
// NFC radio while LF teardown is in flight wedges the firmware hard enough
// to take USB down with it.
static void lf_scan_stop(RfidBackend* self) {
    LfImpl* impl = self->impl;
    if(impl->reading) {
        lfrfid_worker_stop(impl->worker);
        impl->reading = false;
    }
    if(impl->thread_running) {
        lfrfid_worker_stop_thread(impl->worker);
        impl->thread_running = false;
    }
}

static void lf_read(RfidBackend* self, RfidReadCb cb, void* ctx) {
    LfImpl* impl = self->impl;
    impl->read_cb = cb;
    impl->read_ctx = ctx;
    lf_scan_stop(self); // joins the worker thread before touching the dict

    size_t size = protocol_dict_get_data_size(impl->dict, impl->protocol);
    if(size > impl->id_buf_size) size = impl->id_buf_size; // cannot happen; belt and braces
    protocol_dict_get_data(impl->dict, impl->protocol, impl->id_buf, size);
    impl->id_len = size;

    const char* name = protocol_dict_get_name(impl->dict, impl->protocol);
    FuriString* hex = furi_string_alloc();
    for(size_t i = 0; i < impl->id_len; i++) {
        furi_string_cat_printf(hex, i ? " %02X" : "%02X", (unsigned)impl->id_buf[i]);
    }
    FURI_LOG_I(TAG, "LF read: %s, ID %s", name ? name : "Unknown", furi_string_get_cstr(hex));
    furi_string_free(hex);

    // Synchronous callback is safe: it only pushes onto the dispatcher's
    // message queue, the same thing the GUI-thread menu callback does.
    impl->read_cb(impl->read_ctx);
}

static uint32_t lf_read_timeout_ms(RfidBackend* self) {
    UNUSED(self);
    return 0; // read already completed synchronously; the core arms no timer
}

static const char* lf_card_name(RfidBackend* self) {
    LfImpl* impl = self->impl;
    const char* name = protocol_dict_get_name(impl->dict, impl->protocol);
    return name ? name : "Unknown";
}

static void lf_describe(RfidBackend* self, FuriString* out) {
    LfImpl* impl = self->impl;
    const char* name = protocol_dict_get_name(impl->dict, impl->protocol);
    const char* mfr = protocol_dict_get_manufacturer(impl->dict, impl->protocol);

    furi_string_cat_printf(out, "Band: 125 kHz LF\n");
    furi_string_cat_printf(out, "Type: %s\n", name ? name : "Unknown");
    if(mfr && mfr[0] != '\0') {
        furi_string_cat_printf(out, "Mfr: %s\n", mfr);
    }
    furi_string_cat_str(out, "ID: ");
    for(size_t i = 0; i < impl->id_len; i++) {
        furi_string_cat_printf(out, i ? " %02X" : "%02X", (unsigned)impl->id_buf[i]);
    }
    furi_string_cat_str(out, "\n");

    // Render into a scratch string first: a protocol may have no renderer at
    // all, and protocol_dict_render_data() NULL-checks the function pointer
    // internally (verified against firmware 1.4.3 source), so this cannot
    // crash - the empty-string test is what detects "no renderer".
    FuriString* tmp = furi_string_alloc();
    protocol_dict_render_data(impl->dict, tmp, impl->protocol);
    if(furi_string_size(tmp) == 0) {
        protocol_dict_render_brief_data(impl->dict, tmp, impl->protocol);
    }
    if(furi_string_size(tmp) > 0) {
        furi_string_cat_str(out, "\n");
        furi_string_cat(out, tmp);
        furi_string_cat_str(out, "\n");
    }
    furi_string_free(tmp);
}

static LfImpl lf_impl;

static RfidBackend lf_backend = {
    .name = "125 kHz LF",
    .band_label = "125 kHz LF",
    .band = RfidBandLf,
    .scan_ms = LF_PHASE_MS,
    .available = lf_available,
    .alloc = lf_alloc,
    .release = lf_release,
    .scan_start = lf_scan_start,
    .scan_stop = lf_scan_stop,
    .read = lf_read,
    .read_timeout_ms = lf_read_timeout_ms,
    .describe = lf_describe,
    .card_name = lf_card_name,
    .impl = &lf_impl,
};

RfidBackend* rfid_backend_lf(void) {
    return &lf_backend;
}
