/*
 * 13.56 MHz HF/NFC backend.
 *
 * Ported from universal_card_reader/reader_nfc.c (scan/poll) and
 * card_info.c (report rendering), merged into one RfidBackend. No save, no
 * emulation, no EMV data chain - only a one-shot PPSE presence probe.
 */
#include "backend_hf.h"
#include "rfid_app.h"

#include <stdarg.h>
#include <stdio.h>

#include <nfc/nfc.h>
#include <nfc/nfc_scanner.h>
#include <nfc/nfc_poller.h>
#include <nfc/nfc_device.h>
#include <nfc/protocols/nfc_protocol.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>
#include <nfc/protocols/iso14443_3b/iso14443_3b_poller.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>
#include <nfc/protocols/felica/felica_poller.h>
#include <nfc/protocols/st25tb/st25tb_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_classic/mf_classic_poller.h>
#include <toolbox/bit_buffer.h>

/* ------------------------------ state ----------------------------------- */

typedef struct {
    Nfc* nfc;
    NfcScanner* scanner;
    NfcPoller* poller;
    NfcDevice* device;
    NfcProtocol display_protocol; // most-derived, for the name and the chain
    NfcProtocol poll_protocol; // what the poller actually runs; ids 0..11 only
    bool emv_present;
    uint8_t mfc_pass; // 0 = key A pass, 1 = key B pass
    uint8_t mfc_sector; // next sector to offer a key for
    RfidDetectCb detect_cb;
    void* detect_ctx;
    RfidReadCb read_cb;
    void* read_ctx;
} HfImpl;

/*
 * NfcProtocolNum and NfcProtocolInvalid are NOT stable across firmware forks
 * (Momentum appends Ntag4xx/Type4Tag/Emv, moving both). The tables below sit
 * at the head of the enum and have the same values on official and Momentum
 * (ids 0..11 verified), so relationships are resolved with
 * nfc_protocol_has_parent() - evaluated by the firmware, against ids we know
 * are valid - and sentinels are never touched.
 */
static const NfcProtocol hf_base_protocols[] = {
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolSt25tb,
};

// True for a transport protocol, false for anything layered on top of one.
static bool hf_protocol_is_base(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(hf_base_protocols); i++) {
        if(p == hf_base_protocols[i]) return true;
    }
    return false;
}

// Pollable protocols, most-derived first. Anything the scanner returns that is
// not in this list (Ntag4xx/Type4Tag/Emv on Momentum, future fork additions)
// resolves to an ancestor from this list and never reaches nfc_poller_alloc.
// Iso14443_4b and Slix are deliberately absent: their poller event enums are
// unverified, so those cards fall back to their transports (3b / ISO15693-3).
static const NfcProtocol hf_pollable_protocols[] = {
    NfcProtocolMfUltralight,
    NfcProtocolMfClassic,
    NfcProtocolIso14443_4a,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolSt25tb,
};

// The most-derived protocol we can safely poll for `p`.
static NfcProtocol hf_poll_protocol(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(hf_pollable_protocols); i++) {
        NfcProtocol q = hf_pollable_protocols[i];
        if(p == q || nfc_protocol_has_parent(p, q)) return q;
    }
    return NfcProtocolIso14443_3a; // unreachable for scanner output
}

// Most-derived-first, for the "Chain:" line. Walking toward a sentinel is the
// defect class that hung this repo's first app on Momentum, so this table is
// iterated and every entry is checked against the firmware with
// nfc_protocol_has_parent() - nfc_protocol_get_parent() is never called.
static const NfcProtocol hf_chain_order[] = {
    NfcProtocolMfDesfire,
    NfcProtocolMfPlus,
    NfcProtocolMfClassic,
    NfcProtocolMfUltralight,
    NfcProtocolSlix,
    NfcProtocolIso14443_4a,
    NfcProtocolIso14443_4b,
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolSt25tb,
};

/* ----------------------------- report helpers ---------------------------- */

// furi_string_vcat_printf is not in the linkable API, so format through a
// stack buffer instead.
static void out_addf(FuriString* out, const char* fmt, ...) {
    // The marker is the state: once "[truncated]" is the tail of `out`, every
    // later call is a no-op. Derived from the output, so no file-scope flag and
    // no per-render reset. `out` is always empty at the start of a render
    // (reader_report_begin()/rfid_report_begin() call furi_string_reset()).
    if(furi_string_size(out) >= CARD_INFO_MAX) {
        if(!furi_string_end_with_str(out, "[truncated]")) {
            furi_string_cat_str(out, "\n[truncated]");
        }
        return;
    }
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    furi_string_cat_str(out, buf);
}

// "AA BB CC" space-separated.
static void out_hex(FuriString* out, const uint8_t* data, size_t len) {
    for(size_t i = 0; i < len; i++) {
        out_addf(out, i ? " %02X" : "%02X", (unsigned)data[i]);
    }
}

/*
 * nfc_device_get_data() crashes when the requested layer is not part of the
 * device's protocol hierarchy, so every section below is guarded by this
 * first. Every `layer` passed in is a compile-time id 0..11 - valid on
 * official and Momentum alike, so the firmware-side furi_check in
 * nfc_protocol_has_parent() cannot trip.
 */
static bool dev_has(const NfcDevice* d, NfcProtocol layer) {
    NfcProtocol p = nfc_device_get_protocol(d);
    return p == layer || nfc_protocol_has_parent(p, layer);
}

/* --------------------------- payment-card probe -------------------------- */

// SELECT PPSE ("2PAY.SYS.DDF01") - the contactless directory every EMV card
// answers. Status word only; no record is ever read, so no PAN, expiry or
// cardholder name is requested from the card.
static const uint8_t k_ppse_apdu[] = {
    0x00,
    0xA4,
    0x04,
    0x00,
    0x0E,
    0x32,
    0x50,
    0x41,
    0x59,
    0x2E,
    0x53,
    0x59,
    0x53,
    0x2E,
    0x44,
    0x44,
    0x46,
    0x30,
    0x31,
    0x00,
};

static bool hf_probe_emv(Iso14443_4aPoller* poller) {
    BitBuffer* tx = bit_buffer_alloc(32);
    BitBuffer* rx = bit_buffer_alloc(256);
    bool found = false;

    bit_buffer_reset(tx);
    bit_buffer_copy_bytes(tx, k_ppse_apdu, sizeof(k_ppse_apdu));
    if(iso14443_4a_poller_send_block(poller, tx, rx) == Iso14443_4aErrorNone) {
        size_t n = bit_buffer_get_size_bytes(rx);
        if(n >= 2) {
            const uint8_t* d = bit_buffer_get_data(rx);
            uint16_t sw = ((uint16_t)d[n - 2] << 8) | d[n - 1];
            // 9000 = OK. 61xx = OK with more data available; both mean the
            // card has a payment directory. Neither branch reads that data.
            found = (sw == 0x9000) || ((sw & 0xFF00u) == 0x6100u);
        }
    }
    bit_buffer_free(tx);
    bit_buffer_free(rx);
    return found;
}

/* ------------------------------- poller ----------------------------------- */

// Common tail of every poller path: snapshot the card into the device (a deep
// copy, so the data outlives the poller) and hand over to the GUI thread.
// Runs on the NFC worker thread.
static NfcCommand hf_done(HfImpl* impl, NfcProtocol polled) {
    nfc_device_set_data(impl->device, polled, nfc_poller_get_data(impl->poller));
    FURI_LOG_I(TAG, "HF read: %s", nfc_device_get_protocol_name(impl->display_protocol));
    impl->read_cb(impl->read_ctx);
    return NfcCommandStop;
}

// Runs on the NFC worker thread. The callback must answer the poller's
// requests (mode, keys, auth context) and stop on a terminal event; the exact
// set differs per protocol. Only compile-time protocol ids 0..11 ever reach
// this switch (see hf_poll_protocol).
static NfcCommand hf_poller_callback(NfcGenericEvent event, void* context) {
    HfImpl* impl = context;

    switch(event.protocol) {
    case NfcProtocolIso14443_3a:
        if(((Iso14443_3aPollerEvent*)event.event_data)->type == Iso14443_3aPollerEventTypeReady) {
            return hf_done(impl, event.protocol);
        }
        break;
    case NfcProtocolIso14443_3b:
        if(((Iso14443_3bPollerEvent*)event.event_data)->type == Iso14443_3bPollerEventTypeReady) {
            return hf_done(impl, event.protocol);
        }
        break;
    case NfcProtocolIso15693_3:
        // Ready means inventory + system info + all blocks were read already.
        if(((Iso15693_3PollerEvent*)event.event_data)->type == Iso15693_3PollerEventTypeReady) {
            return hf_done(impl, event.protocol);
        }
        break;
    case NfcProtocolSt25tb:
        if(((St25tbPollerEvent*)event.event_data)->type == St25tbPollerEventTypeReady) {
            return hf_done(impl, event.protocol);
        }
        break;
    case NfcProtocolFelica: {
        FelicaPollerEvent* e = event.event_data;
        switch(e->type) {
        case FelicaPollerEventTypeReady:
        case FelicaPollerEventTypeIncomplete: // partial dump still worth showing
            return hf_done(impl, event.protocol);
        case FelicaPollerEventTypeRequestAuthContext:
            // alloc does not initialise skip_auth (malloc garbage) and the
            // activate handler branches on it.
            e->data->auth_context->skip_auth = true;
            break;
        default:
            break; // Error: keep polling until the timeout
        }
        break;
    }
    case NfcProtocolIso14443_4a:
        if(((Iso14443_4aPollerEvent*)event.event_data)->type == Iso14443_4aPollerEventTypeReady) {
            // iso14443_4a_poller_send_block() is only legal inside the callback.
            impl->emv_present = hf_probe_emv((Iso14443_4aPoller*)event.instance);
            return hf_done(impl, event.protocol);
        }
        break;
    case NfcProtocolMfUltralight: {
        MfUltralightPollerEvent* e = event.event_data;
        switch(e->type) {
        case MfUltralightPollerEventTypeAuthRequest:
            // The firmware reads skip_auth uninitialised otherwise (verified).
            e->data->auth_context.skip_auth = true;
            break;
        case MfUltralightPollerEventTypeReadSuccess:
        case MfUltralightPollerEventTypeReadFailed: // partial data still worth showing
            return hf_done(impl, event.protocol);
        default:
            break; // RequestMode: the firmware pre-sets Read
        }
        break;
    }
    case NfcProtocolMfClassic: {
        MfClassicPollerEvent* e = event.event_data;
        switch(e->type) {
        case MfClassicPollerEventTypeRequestMode:
            // The firmware furi_crash()es on an uninitialised mode. Read mode
            // only: the DictAttack modes are ABI-unsafe on Momentum.
            e->data->poller_mode.mode = MfClassicPollerModeRead;
            e->data->poller_mode.data = NULL;
            break;
        case MfClassicPollerEventTypeRequestReadSector: {
            // Offer the factory-default transport key (FF FF FF FF FF FF, the
            // NXP shipping default, for reading your own blank/personal cards)
            // for every sector: key A pass first, then a key B pass over the
            // sectors still unread. key_provided = false ends the read; the
            // firmware then emits Success. Never touch key_request_data (only
            // used by dict-attack modes, ABI-unsafe on Momentum).
            MfClassicPollerEventDataReadSectorRequest* r = &e->data->read_sector_request_data;
            static const uint8_t transport_key[MF_CLASSIC_KEY_SIZE] =
                {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            const MfClassicData* d = (const MfClassicData*)nfc_poller_get_data(impl->poller);
            uint8_t sectors = mf_classic_get_total_sectors_num(d->type);
            for(;;) {
                if(impl->mfc_sector >= sectors) {
                    if(impl->mfc_pass == 0) {
                        impl->mfc_pass = 1;
                        impl->mfc_sector = 0;
                        continue;
                    }
                    r->key_provided = false;
                    break;
                }
                if(impl->mfc_pass == 1 && mf_classic_is_sector_read(d, impl->mfc_sector)) {
                    impl->mfc_sector++;
                    continue;
                }
                r->sector_num = impl->mfc_sector++;
                memcpy(r->key.data, transport_key, MF_CLASSIC_KEY_SIZE);
                r->key_type = impl->mfc_pass == 0 ? MfClassicKeyTypeA : MfClassicKeyTypeB;
                r->key_provided = true;
                break;
            }
            break;
        }
        case MfClassicPollerEventTypeSuccess:
        case MfClassicPollerEventTypeFail:
            return hf_done(impl, event.protocol);
        default:
            break; // CardDetected/CardLost/DataUpdate/...: keep polling
        }
        break;
    }
    default:
        break;
    }

    return NfcCommandContinue; // keep polling until a terminal event or the timeout
}

/* ------------------------------- scanner ----------------------------------- */

// Runs on the NFC worker thread.
static void hf_scanner_callback(NfcScannerEvent event, void* context) {
    HfImpl* impl = context;
    if(event.type != NfcScannerEventTypeDetected || event.data.protocol_num == 0) {
        return;
    }

    // The scanner already prefers children over their parents; picking the
    // first non-transport entry gives the friendly name (e.g. "Mifare Classic")
    // rather than the transport underneath it.
    NfcProtocol best = event.data.protocols[0];
    for(size_t i = 0; i < event.data.protocol_num; i++) {
        if(!hf_protocol_is_base(event.data.protocols[i])) {
            best = event.data.protocols[i];
            break;
        }
    }

    impl->display_protocol = best;
    impl->poll_protocol = hf_poll_protocol(best);
    FURI_LOG_I(TAG, "HF detected: %s", nfc_device_get_protocol_name(best));
    impl->detect_cb(impl->detect_ctx);
}

/* --------------------------------- vtable ---------------------------------- */

static bool hf_available(void) {
    return true;
}

static void hf_alloc(RfidBackend* self) {
    HfImpl* impl = self->impl;
    impl->nfc = nfc_alloc();
    impl->device = nfc_device_alloc();
}

static void hf_release(RfidBackend* self) {
    HfImpl* impl = self->impl;
    nfc_device_free(impl->device);
    nfc_free(impl->nfc);
}

static void hf_scan_start(RfidBackend* self, RfidDetectCb cb, void* ctx) {
    HfImpl* impl = self->impl;
    impl->emv_present = false;
    impl->mfc_pass = 0;
    impl->mfc_sector = 0;
    impl->detect_cb = cb;
    impl->detect_ctx = ctx;
    impl->scanner = nfc_scanner_alloc(impl->nfc);
    nfc_scanner_start(impl->scanner, hf_scanner_callback, impl);
}

static void hf_scan_stop(RfidBackend* self) {
    HfImpl* impl = self->impl;
    if(impl->poller) {
        nfc_poller_stop(impl->poller);
        nfc_poller_free(impl->poller);
        impl->poller = NULL;
    }
    if(impl->scanner) {
        nfc_scanner_stop(impl->scanner);
        nfc_scanner_free(impl->scanner);
        impl->scanner = NULL;
    }
}

static void hf_read(RfidBackend* self, RfidReadCb cb, void* ctx) {
    HfImpl* impl = self->impl;
    impl->read_cb = cb;
    impl->read_ctx = ctx;
    if(impl->scanner) {
        nfc_scanner_stop(impl->scanner);
        nfc_scanner_free(impl->scanner);
        impl->scanner = NULL;
    }
    impl->poller = nfc_poller_alloc(impl->nfc, impl->poll_protocol);
    nfc_poller_start(impl->poller, hf_poller_callback, impl);
}

// Per-protocol read bound: in-callback work (PPSE probe, Classic key passes,
// ISO15693 full block dump) needs longer than a bare transport activation.
// Per-protocol read bounds. Same values and rationale as universal_card_reader's
// reader_app.h:28-33; duplicated rather than shared because they describe this
// backend's poller work, which the vtable deliberately hides from the app layer.
#define HF_MFC_READ_TIMEOUT_MS        12000 // 2 key passes x up to 80 sector requests
#define HF_MFUL_READ_TIMEOUT_MS       8000  // full page dump inside the poller callback
#define HF_ISO15693_READ_TIMEOUT_MS   8000  // full block dump inside activate
#define HF_FELICA_READ_TIMEOUT_MS     6000  // system/service enumeration
#define HF_ISO14443_4A_READ_TIMEOUT_MS 3000 // one PPSE APDU now, not the whole EMV chain
#define HF_READ_TIMEOUT_MS            2500  // plain transports: activation only
static uint32_t hf_read_timeout_ms(RfidBackend* self) {
    HfImpl* impl = self->impl;
    switch(impl->poll_protocol) {
    case NfcProtocolMfClassic:
        return HF_MFC_READ_TIMEOUT_MS;
    case NfcProtocolMfUltralight:
        return HF_MFUL_READ_TIMEOUT_MS;
    case NfcProtocolIso15693_3:
        return HF_ISO15693_READ_TIMEOUT_MS;
    case NfcProtocolFelica:
        return HF_FELICA_READ_TIMEOUT_MS;
    case NfcProtocolIso14443_4a:
        return HF_ISO14443_4A_READ_TIMEOUT_MS;
    default:
        return HF_READ_TIMEOUT_MS;
    }
}

static const char* hf_card_name(RfidBackend* self) {
    HfImpl* impl = self->impl;
    return nfc_device_get_protocol_name(impl->display_protocol);
}

/* ------------------------------ report sections ----------------------------- */

static void hf_describe_iso14443_3a(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolIso14443_3a)) return;
    const Iso14443_3aData* d =
        (const Iso14443_3aData*)nfc_device_get_data(device, NfcProtocolIso14443_3a);
    uint8_t atqa[2];
    iso14443_3a_get_atqa(d, atqa);
    out_addf(out, "\n[ISO14443-3A]\n");
    out_addf(out, "ATQA: ");
    out_hex(out, atqa, sizeof(atqa));
    out_addf(out, "\n");
    out_addf(out, "SAK: 0x%02X\n", (unsigned)iso14443_3a_get_sak(d));
}

static void hf_describe_iso14443_4a(FuriString* out, const NfcDevice* device, bool emv_present) {
    if(!dev_has(device, NfcProtocolIso14443_4a)) return;
    const Iso14443_4aData* d =
        (const Iso14443_4aData*)nfc_device_get_data(device, NfcProtocolIso14443_4a);
    out_addf(out, "\n[ISO14443-4A]\n");
    uint32_t count = 0;
    const uint8_t* hist = iso14443_4a_get_historical_bytes(d, &count);
    if(count > 0) {
        out_addf(out, "ATS hist: ");
        out_hex(out, hist, count);
        out_addf(out, "\n");
    }
    out_addf(out, "Frame max: %u\n", (unsigned)iso14443_4a_get_frame_size_max(d));
    if(emv_present) {
        out_addf(out, "Payment: EMV application present\n");
        out_addf(out, "(bank card - PAN/expiry/name deliberately not read)\n");
    }
}

// New: universal_card_reader's renderer set has no ISO14443-3B section at all.
static void hf_describe_iso14443_3b(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolIso14443_3b)) return;
    const Iso14443_3bData* d =
        (const Iso14443_3bData*)nfc_device_get_data(device, NfcProtocolIso14443_3b);
    out_addf(out, "\n[ISO14443-3B]\n");
    size_t len = 0;
    const uint8_t* app_data = iso14443_3b_get_application_data(d, &len);
    out_addf(out, "App data: ");
    out_hex(out, app_data, len);
    out_addf(out, "\n");
    out_addf(out, "Frame max: %u\n", (unsigned)iso14443_3b_get_frame_size_max(d));
}

static void hf_describe_iso15693_3(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolIso15693_3)) return;
    const Iso15693_3Data* d =
        (const Iso15693_3Data*)nfc_device_get_data(device, NfcProtocolIso15693_3);
    out_addf(out, "\n[ISO15693-3]\n");
    out_addf(out, "Mfr: 0x%02X\n", (unsigned)iso15693_3_get_manufacturer_id(d));
    if(d->system_info.flags & ISO15693_3_SYSINFO_FLAG_DSFID) {
        out_addf(out, "DSFID: 0x%02X\n", (unsigned)d->system_info.dsfid);
    }
    if(d->system_info.flags & ISO15693_3_SYSINFO_FLAG_AFI) {
        out_addf(out, "AFI: 0x%02X\n", (unsigned)d->system_info.afi);
    }
    if(d->system_info.flags & ISO15693_3_SYSINFO_FLAG_IC_REF) {
        out_addf(out, "IC ref: 0x%02X\n", (unsigned)d->system_info.ic_ref);
    }
    uint16_t blocks = iso15693_3_get_block_count(d);
    uint8_t block_size = iso15693_3_get_block_size(d);
    out_addf(out, "Blocks: %u x %u bytes\n", (unsigned)blocks, (unsigned)block_size);
    if(blocks == 0) return; // block_data SimpleArray is uninitialised then
    uint16_t limit = blocks > 255 ? 255 : blocks; // the block getters take uint8_t
    for(uint16_t i = 0; i < limit; i++) {
        out_addf(out, "BLK %02u: ", (unsigned)i);
        out_hex(out, iso15693_3_get_block_data(d, (uint8_t)i), block_size);
        if(iso15693_3_is_block_locked(d, (uint8_t)i)) out_addf(out, " *");
        out_addf(out, "\n");
    }
}

static void hf_describe_felica(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolFelica)) return;
    const FelicaData* d = (const FelicaData*)nfc_device_get_data(device, NfcProtocolFelica);
    out_addf(out, "\n[FeliCa]\n");
    out_addf(out, "IDm: ");
    out_hex(out, d->idm.data, FELICA_IDM_SIZE);
    out_addf(out, "\n");
    out_addf(out, "PMm: ");
    out_hex(out, d->pmm.data, FELICA_PMM_SIZE);
    out_addf(out, "\n");
    out_addf(out, "Blocks: %u/%u read\n", (unsigned)d->blocks_read, (unsigned)d->blocks_total);
}

// New: universal_card_reader's renderer set has no ST25TB section at all.
static void hf_describe_st25tb(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolSt25tb)) return;
    const St25tbData* d = (const St25tbData*)nfc_device_get_data(device, NfcProtocolSt25tb);
    out_addf(out, "\n[ST25TB]\n");
    out_addf(out, "Type: %s\n", st25tb_get_device_name(d, NfcDeviceNameTypeFull));
    uint8_t count = st25tb_get_block_count(d->type);
    out_addf(out, "Blocks: %u\n", (unsigned)count);
    uint8_t limit = count > ST25TB_MAX_BLOCKS ? ST25TB_MAX_BLOCKS : count;
    for(uint8_t i = 0; i < limit; i++) {
        out_addf(out, "BLK %02u: %08lX\n", (unsigned)i, (unsigned long)d->blocks[i]);
    }
    out_addf(out, "OTP: %08lX\n", (unsigned long)d->system_otp_block);
}

static void hf_describe_mf_ultralight(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolMfUltralight)) return;
    const MfUltralightData* d =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    out_addf(out, "\n[Mifare Ultralight]\n");
    out_addf(out, "Type: %s\n", mf_ultralight_get_device_name(d, NfcDeviceNameTypeFull));
    out_addf(out, "Pages: %u/%u\n", (unsigned)d->pages_read, (unsigned)d->pages_total);
    uint16_t limit =
        d->pages_read < MF_ULTRALIGHT_MAX_PAGE_NUM ? d->pages_read : MF_ULTRALIGHT_MAX_PAGE_NUM;
    for(uint16_t i = 0; i < limit; i++) {
        out_addf(out, "P %02u: ", (unsigned)i);
        out_hex(out, d->page[i].data, MF_ULTRALIGHT_PAGE_SIZE);
        out_addf(out, "\n");
    }
    // NDEF decoding is deliberately out of scope for this app.
}

static void hf_describe_mf_classic(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolMfClassic)) return;
    const MfClassicData* d =
        (const MfClassicData*)nfc_device_get_data(device, NfcProtocolMfClassic);
    out_addf(out, "\n[Mifare Classic]\n");
    out_addf(out, "Type: %s\n", mf_classic_get_device_name(d, NfcDeviceNameTypeFull));

    uint8_t sectors = mf_classic_get_total_sectors_num(d->type);
    uint8_t sectors_read = 0;
    for(uint8_t s = 0; s < sectors; s++) {
        if(mf_classic_is_sector_read(d, s)) sectors_read++;
    }
    out_addf(out, "Sectors: %u/%u read\n", (unsigned)sectors_read, (unsigned)sectors);

    // Sector map, 8 sectors per line: '+' all blocks read, '~' partial, '.' nothing.
    for(uint8_t base = 0; base < sectors; base += 8) {
        uint8_t last = base + 7 < sectors ? base + 7 : sectors - 1;
        out_addf(out, "Sectors %02u-%02u: ", (unsigned)base, (unsigned)last);
        for(uint8_t s = base; s <= last; s++) {
            uint8_t first = mf_classic_get_first_block_num_of_sector(s);
            uint8_t blk_count = mf_classic_get_blocks_num_in_sector(s);
            uint8_t read = 0;
            for(uint8_t b = 0; b < blk_count; b++) {
                if(mf_classic_is_block_read(d, first + b)) read++;
            }
            out_addf(out, "%c", read == 0 ? '.' : (read == blk_count ? '+' : '~'));
        }
        out_addf(out, "\n");
    }

    uint16_t total_blocks = mf_classic_get_total_block_num(d->type);
    if(total_blocks > 255) total_blocks = 255; // mf_classic_is_block_read takes uint8_t
    for(uint16_t b = 0; b < total_blocks; b++) {
        if(mf_classic_is_block_read(d, (uint8_t)b)) {
            out_addf(out, "B %03u: ", (unsigned)b);
            out_hex(out, d->block[b].data, MF_CLASSIC_BLOCK_SIZE);
            out_addf(out, "\n");
        }
    }
}

// MfDesfire and MfPlus need no renderer of their own: neither is in
// hf_pollable_protocols, so both resolve to Iso14443_4a and their data
// layers are never present on the device.
static void hf_describe(RfidBackend* self, FuriString* out) {
    HfImpl* impl = self->impl;

    out_addf(out, "Band: 13.56 MHz HF\n");
    out_addf(out, "Type: %s\n", nfc_device_get_protocol_name(impl->display_protocol));

    bool chain_started = false;
    for(size_t i = 0; i < COUNT_OF(hf_chain_order); i++) {
        NfcProtocol q = hf_chain_order[i];
        if(impl->display_protocol != q && !nfc_protocol_has_parent(impl->display_protocol, q)) {
            continue;
        }
        out_addf(out, chain_started ? " <- " : "Chain: ");
        out_addf(out, "%s", nfc_device_get_protocol_name(q));
        chain_started = true;
    }
    if(chain_started) out_addf(out, "\n");

    size_t uid_len = 0;
    const uint8_t* uid = nfc_device_get_uid(impl->device, &uid_len);
    out_addf(out, "UID: ");
    out_hex(out, uid, uid_len);
    out_addf(out, "\n");

    hf_describe_iso14443_3a(out, impl->device);
    hf_describe_iso14443_4a(out, impl->device, impl->emv_present);
    hf_describe_iso14443_3b(out, impl->device);
    hf_describe_iso15693_3(out, impl->device);
    hf_describe_felica(out, impl->device);
    hf_describe_st25tb(out, impl->device);
    hf_describe_mf_ultralight(out, impl->device);
    hf_describe_mf_classic(out, impl->device);
}

/* -------------------------------- factory ---------------------------------- */

static HfImpl hf_impl;

static RfidBackend hf_backend = {
    .name = "13.56 MHz HF",
    .band_label = "13.56 MHz HF",
    .band = RfidBandHf,
    .scan_ms = HF_PHASE_MS,
    .available = hf_available,
    .alloc = hf_alloc,
    .release = hf_release,
    .scan_start = hf_scan_start,
    .scan_stop = hf_scan_stop,
    .read = hf_read,
    .read_timeout_ms = hf_read_timeout_ms,
    .describe = hf_describe,
    .card_name = hf_card_name,
    .impl = &hf_impl,
};

RfidBackend* rfid_backend_hf(void) {
    return &hf_backend;
}
