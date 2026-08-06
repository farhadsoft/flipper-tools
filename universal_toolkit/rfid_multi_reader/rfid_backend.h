#pragma once

#include <furi.h>

typedef struct RfidBackend RfidBackend;

/**
 * Fired from a radio worker thread. The implementation of these callbacks lives
 * in the core and does nothing but view_dispatcher_send_custom_event(). A backend
 * MUST NOT touch hardware from the thread that invokes them.
 */
typedef void (*RfidDetectCb)(void* ctx);
typedef void (*RfidReadCb)(void* ctx);

typedef enum {
    RfidBandHf, // 13.56 MHz, built-in
    RfidBandLf, // 125 kHz, built-in
    RfidBandCount,
} RfidBand;

struct RfidBackend {
    const char* name; // "13.56 MHz HF" - menu / logs
    const char* band_label; // "< 13.56 MHz HF >" - boxed label on the scan screen
    RfidBand band;
    uint32_t scan_ms; // scan budget for one Auto-mode phase

    /** True when the Flipper can drive this band with built-in hardware. */
    bool (*available)(void);

    /** GUI thread, once per app lifetime. Acquire firmware objects. */
    void (*alloc)(RfidBackend* self);
    /** GUI thread, once per app lifetime. Release them. scan_stop() ran first. */
    void (*release)(RfidBackend* self);

    /** GUI thread. Clear per-card state and begin looking for a card. */
    void (*scan_start)(RfidBackend* self, RfidDetectCb cb, void* ctx);
    /** GUI thread. Idempotent full teardown; joins any worker thread before returning. */
    void (*scan_stop)(RfidBackend* self);
    /** GUI thread, after a detect. Perform the full read; cb fires when it finishes. */
    void (*read)(RfidBackend* self, RfidReadCb cb, void* ctx);
    /** GUI thread, valid after read() returns. Upper bound in ms, or 0 if read()
     *  already completed synchronously and no timeout should be armed. */
    uint32_t (*read_timeout_ms)(RfidBackend* self);
    /** GUI thread, after a read. Append the full human-readable report to `out`. */
    void (*describe)(RfidBackend* self, FuriString* out);
    /** GUI thread, after a read. Short card type for the report header. */
    const char* (*card_name)(RfidBackend* self);

    void* impl; // backend-private state, owned by alloc()/release()
};

RfidBackend* rfid_backend_hf(void);
RfidBackend* rfid_backend_lf(void);
