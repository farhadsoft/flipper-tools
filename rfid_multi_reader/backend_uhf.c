/*
 * UHF (860-960 MHz) stub backend.
 *
 * The Flipper has no built-in UHF hardware. This is a complete vtable purely
 * so the core never has to NULL-check a slot: available() is false, so
 * rfid_build_rotation() never selects it and none of the no-op bodies below
 * is ever called in practice. They exist so a future GPIO/UART UHF
 * implementation is a drop-in body swap with zero core changes.
 */
#include "backend_uhf.h"

static bool uhf_available(void) {
    return false;
}

static void uhf_alloc(RfidBackend* self) {
    UNUSED(self);
}

static void uhf_release(RfidBackend* self) {
    UNUSED(self);
}

static void uhf_scan_start(RfidBackend* self, RfidDetectCb cb, void* ctx) {
    UNUSED(self);
    UNUSED(cb);
    UNUSED(ctx);
}

static void uhf_scan_stop(RfidBackend* self) {
    UNUSED(self);
}

static void uhf_read(RfidBackend* self, RfidReadCb cb, void* ctx) {
    UNUSED(self);
    UNUSED(cb);
    UNUSED(ctx);
}

static uint32_t uhf_read_timeout_ms(RfidBackend* self) {
    UNUSED(self);
    return 0;
}

static const char* uhf_card_name(RfidBackend* self) {
    UNUSED(self);
    return "UHF";
}

static void uhf_describe(RfidBackend* self, FuriString* out) {
    UNUSED(self);
    furi_string_cat_str(out, "Band: UHF 860-960 MHz\n");
    furi_string_cat_str(out, "External module required - not built into Flipper.\n");
}

static RfidBackend uhf_backend = {
    .name = "UHF 860-960 MHz",
    .band_label = "< UHF 860-960 >",
    .band = RfidBandUhf,
    .scan_ms = 0,
    .available = uhf_available,
    .alloc = uhf_alloc,
    .release = uhf_release,
    .scan_start = uhf_scan_start,
    .scan_stop = uhf_scan_stop,
    .read = uhf_read,
    .read_timeout_ms = uhf_read_timeout_ms,
    .describe = uhf_describe,
    .card_name = uhf_card_name,
    .impl = NULL,
};

RfidBackend* rfid_backend_uhf(void) {
    return &uhf_backend;
}
