#include "reader_nfc.h"
#include "reader_ui.h"
#include "card_info.h"
#include "emv.h"
#include "mfc_key_recovery.h"
#undef TAG
#include "../toolkit_app.h"
#undef TAG
#define TAG "UniCardReader"

#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>
#include <nfc/protocols/iso14443_3b/iso14443_3b_poller.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>
#include <nfc/protocols/felica/felica_poller.h>
#include <nfc/protocols/st25tb/st25tb_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_classic/mf_classic_poller.h>

/*
 * NfcProtocolNum and NfcProtocolInvalid are NOT stable across firmware forks:
 * Momentum appends Ntag4xx/Type4Tag/Emv, which moves Invalid from 12 to 16. A
 * FAP compiled against one fork therefore cannot compare a protocol id the
 * running firmware returned against its own copy of those sentinels, and
 * feeding the mismatched value back into nfc_poller_alloc() trips
 * furi_check(protocol < NfcProtocolNum).
 *
 * The protocols below sit at the head of the enum and have the same values on
 * official and Momentum (ids 0..11 verified), so relationships are resolved
 * with nfc_protocol_has_parent() — evaluated by the firmware, against ids we
 * know are valid — and sentinels are never touched.
 */
static const NfcProtocol reader_base_protocols[] = {
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolSt25tb,
};

// True for a transport protocol, false for anything layered on top of one.
static bool protocol_is_base(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(reader_base_protocols); i++) {
        if(p == reader_base_protocols[i]) return true;
    }
    return false;
}

// Pollable protocols, most-derived first. Anything the scanner returns that is
// not in this list (Ntag4xx/Type4Tag/Emv on Momentum, future fork additions)
// resolves to an ancestor from this list and never reaches nfc_poller_alloc.
// Iso14443_4b and Slix are deliberately absent: their poller event enums are
// unverified, so those cards fall back to their transports (3b / ISO15693-3).
static const NfcProtocol reader_pollable_protocols[] = {
    NfcProtocolMfUltralight,
    NfcProtocolMfClassic,
    NfcProtocolIso14443_4a,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolSt25tb,
};

// The most-derived protocol we can safely poll for `p`. Declared in
// reader_nfc.h: reader_do_load() reuses it so a loaded .nfc file's
// poll_protocol satisfies the same invariant as a live scan's.
NfcProtocol reader_poll_protocol(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(reader_pollable_protocols); i++) {
        NfcProtocol q = reader_pollable_protocols[i];
        if(p == q || nfc_protocol_has_parent(p, q)) return q;
    }
    return NfcProtocolIso14443_3a; // unreachable: every protocol id's chain ends at a base transport, all 5 of which are in the list above
}

// Protocols nfc_listener_alloc() can actually emulate. Two things must both
// hold, checked against official firmware 1.4.3 (API 87.1): the protocol's
// own entry in nfc_listeners_api[] must be non-NULL
// (nfc_listener_alloc furi_check()s exactly that), AND every ancestor's entry
// must be non-NULL too (nfc_listener_list_alloc() walks the whole parent
// chain and calls each ancestor's ->alloc() with no NULL check at all — an
// unchecked crash, not even a furi_check).
//
// Re-verified 2026-08-08: iso14443_3b_listener_alloc and st25tb_listener_alloc
// are NOT exported in api_symbols.csv for official 1.4.3, so those protocols
// remain excluded. Every protocol below either has no parent or descends only
// from Iso14443_3a (non-NULL), so the whole chain is safe for each entry here.
static const NfcProtocol reader_emulatable_protocols[] = {
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_4a,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolMfUltralight,
    NfcProtocolMfClassic,
};

bool reader_protocol_emulatable(NfcProtocol p) {
    for(size_t i = 0; i < COUNT_OF(reader_emulatable_protocols); i++) {
        if(p == reader_emulatable_protocols[i]) return true;
    }
    return false;
}

// Per-protocol read bound: in-callback work (EMV APDU chain, Classic key
// passes, ISO15693 full block dump) needs longer than a bare transport
// activation.
static uint32_t reader_read_timeout_for(NfcProtocol p) {
    switch(p) {
    case NfcProtocolMfClassic:
        return MFC_READ_TIMEOUT_MS;
    case NfcProtocolMfUltralight:
        return MFUL_READ_TIMEOUT_MS;
    case NfcProtocolIso15693_3:
        return ISO15693_READ_TIMEOUT_MS;
    case NfcProtocolFelica:
        return FELICA_READ_TIMEOUT_MS;
    case NfcProtocolIso14443_4a:
        return EMV_READ_TIMEOUT_MS; // APDU chain in-callback
    default:
        return READ_TIMEOUT_MS; // plain transports
    }
}

void reader_stop_nfc(ReaderApp* app) {
    if(app->listener) {
        nfc_listener_stop(app->listener);
        nfc_listener_free(app->listener);
        app->listener = NULL;
    }
    if(app->poller) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
        app->poller = NULL;
    }
    if(app->scanner) {
        nfc_scanner_stop(app->scanner);
        nfc_scanner_free(app->scanner);
        app->scanner = NULL;
    }
}

// Common tail of every poller path: snapshot the card into the device (a deep
// copy, so the data outlives the poller) and hand over to the GUI thread.
// Runs on the NFC worker thread.
static NfcCommand reader_nfc_done(ReaderApp* app, NfcProtocol polled) {
    nfc_device_set_data(app->device, polled, nfc_poller_get_data(app->poller));
    FURI_LOG_I(TAG, "NFC read: %s", nfc_device_get_protocol_name(app->display_protocol));
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(ReaderEventNfcRead, app->toolkit->gen));
    return NfcCommandStop;
}

// Runs on the NFC worker thread. The callback must answer the poller's
// requests (mode, keys, auth context) and stop on a terminal event; the exact
// set differs per protocol. Only compile-time protocol ids 0..11 ever reach
// this switch (see reader_poll_protocol).
static NfcCommand reader_poller_callback(NfcGenericEvent event, void* context) {
    ReaderApp* app = context;

    switch(event.protocol) {
    case NfcProtocolIso14443_3a:
        if(((Iso14443_3aPollerEvent*)event.event_data)->type == Iso14443_3aPollerEventTypeReady) {
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolIso14443_3b:
        if(((Iso14443_3bPollerEvent*)event.event_data)->type == Iso14443_3bPollerEventTypeReady) {
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolIso15693_3:
        // Ready means inventory + system info + all blocks were read already.
        if(((Iso15693_3PollerEvent*)event.event_data)->type == Iso15693_3PollerEventTypeReady) {
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolSt25tb:
        if(((St25tbPollerEvent*)event.event_data)->type == St25tbPollerEventTypeReady) {
            return reader_nfc_done(app, event.protocol);
        }
        break;
    case NfcProtocolFelica: {
        FelicaPollerEvent* e = event.event_data;
        switch(e->type) {
        case FelicaPollerEventTypeReady:
        case FelicaPollerEventTypeIncomplete: // partial dump still worth showing
            return reader_nfc_done(app, event.protocol);
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
            // The scanner's child-protocol detection (Momentum's EMV / Type4Tag
            // pollers) sends SELECT APDUs during detection and frees the poller
            // without halting, leaving the card's application layer in a
            // half-open state. Our own poller re-activates (RATS succeeds),
            // but the card's applet refuses the first APDU → FWT Timeout →
            // sw=0000 → "No EMV app on card".
            //
            // Fix: on the first Ready, halt the card (reset 3a/4a poller state
            // to Idle) and ask the nfc worker for a full field reset. Returning
            // Continue here is wrong: the 4a poller would immediately retry
            // ReadAts on a just-halted card, RATS would FWT-timeout, and the
            // Error → Idle → ReadAts loop would repeat until the read timeout.
            // NfcCommandReset power-cycles the field (~100 ms), so the next
            // PollerReady re-activates the card cleanly and the second Ready
            // runs emv_read().
            if(!app->emv_reactivate) {
                app->emv_reactivate = true;
                iso14443_4a_poller_halt((Iso14443_4aPoller*)event.instance);
                return NfcCommandReset;
            }
            app->emv_reactivate = false;
            // The read-only EMV chain must run here:
            // iso14443_4a_poller_send_block() is only legal inside the callback.
            memset(&app->emv, 0, sizeof(app->emv));
            emv_read((Iso14443_4aPoller*)event.instance, &app->emv);
            return reader_nfc_done(app, event.protocol);
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
            return reader_nfc_done(app, event.protocol);
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
            // for the initial pass; key recovery (if needed) runs as a second
            // poller in DictAttackStandard mode on the GUI thread. We never
            // answer RequestKey here, so Momentum's key_request_data ABI drift
            // is avoided.
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
            const MfClassicData* d = (const MfClassicData*)nfc_poller_get_data(app->poller);
            uint8_t sectors = mf_classic_get_total_sectors_num(d->type);
            for(;;) {
                if(app->mfc_sector >= sectors) {
                    if(app->mfc_pass == 0) {
                        app->mfc_pass = 1;
                        app->mfc_sector = 0;
                        continue;
                    }
                    r->key_provided = false;
                    break;
                }
                if(app->mfc_pass == 1 && mf_classic_is_sector_read(d, app->mfc_sector)) {
                    app->mfc_sector++;
                    continue;
                }
                r->sector_num = app->mfc_sector++;
                memcpy(r->key.data, transport_key, MF_CLASSIC_KEY_SIZE);
                r->key_type = app->mfc_pass == 0 ? MfClassicKeyTypeA : MfClassicKeyTypeB;
                r->key_provided = true;
                break;
            }
            break;
        }
        case MfClassicPollerEventTypeSuccess:
        case MfClassicPollerEventTypeFail: {
            const MfClassicData* d =
                (const MfClassicData*)nfc_poller_get_data(app->poller);
            if(e->type == MfClassicPollerEventTypeSuccess &&
               !mf_classic_is_card_read(d)) {
                uint8_t sectors = mf_classic_get_total_sectors_num(d->type);
                uint8_t read = 0, keys = 0;
                mf_classic_get_read_sectors_and_keys(d, &read, &keys);
                FURI_LOG_I(
                    TAG,
                    "MFC partial read: %u/%u sectors, %u keys found; will attempt recovery",
                    (unsigned)read,
                    (unsigned)sectors,
                    (unsigned)keys);
                app->mfc_recovery_pending = true;
            }
            return reader_nfc_done(app, event.protocol);
        }
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

// Runs on the NFC worker thread.
static void reader_scanner_callback(NfcScannerEvent event, void* context) {
    ReaderApp* app = context;
    if(event.type != NfcScannerEventTypeDetected || event.data.protocol_num == 0) {
        return;
    }

    // The scanner already prefers children over their parents; picking the
    // first non-transport entry gives the friendly name (e.g. "Mifare Classic")
    // rather than the transport underneath it.
    NfcProtocol best = event.data.protocols[0];
    for(size_t i = 0; i < event.data.protocol_num; i++) {
        if(!protocol_is_base(event.data.protocols[i])) {
            best = event.data.protocols[i];
            break;
        }
    }

    app->display_protocol = best;
    app->poll_protocol = reader_poll_protocol(best);
    FURI_LOG_I(TAG, "NFC detected: %s", nfc_device_get_protocol_name(best));
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EVENT_MAKE(ReaderEventNfcScanned, app->toolkit->gen));
}

// NFC emulation listener callback. Runs on the NFC worker thread; the
// listener itself answers reader commands from firmware-side protocol state,
// so there is nothing for the app to do here but keep going until Back stops
// the listener from the GUI thread.
static NfcCommand reader_listener_callback(NfcGenericEvent event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return NfcCommandContinue;
}

// GUI thread only. Bumping the generation invalidates every event still queued
// from the phase we are leaving.
void reader_start_nfc_phase(ReaderApp* app) {
    reader_stop_all(app);
    app->toolkit->gen++;
    app->lf_phase = false;
    reader_set_scanning(app, false);
    // Rescan from the info screen (Back) must land on the scan view again.
    reader_switch_view(app, ReaderViewScan);

    FURI_LOG_D(TAG, "phase: NFC (gen %lu)", (unsigned long)app->toolkit->gen);
    app->scanner = nfc_scanner_alloc(app->nfc);
    nfc_scanner_start(app->scanner, reader_scanner_callback, app);
    furi_timer_start(app->phase_timer, furi_ms_to_ticks(NFC_PHASE_MS));
}

// GUI thread only. Caller (reader_do_emulate()) has already checked
// reader_protocol_emulatable(app->poll_protocol) and that this is not a
// payment card.
void reader_start_nfc_emulation(ReaderApp* app) {
    reader_stop_all(app); // scanner/poller/LF worker released and joined first
    app->toolkit->gen++;
    const NfcDeviceData* data = nfc_device_get_data(app->device, app->poll_protocol);
    app->listener = nfc_listener_alloc(app->nfc, app->poll_protocol, data);
    nfc_listener_start(app->listener, reader_listener_callback, app);
    size_t uid_len = 0;
    const uint8_t* uid = nfc_device_get_uid(app->device, &uid_len);
    FuriString* uid_hex = furi_string_alloc();
    reader_cat_hex(uid_hex, uid, uid_len);
    FURI_LOG_I(
        TAG,
        "emulating NFC: %s, UID %s",
        nfc_device_get_protocol_name(app->display_protocol),
        furi_string_get_cstr(uid_hex));
    furi_string_free(uid_hex);
    reader_enter_emulating(app, nfc_device_get_protocol_name(app->display_protocol));
}

// EMV / bank card. app->emv is populated by emv_read() (called from inside
// reader_poller_callback whenever poll_protocol is ISO14443-4A), so ppse_ok /
// aid_count is the primary, reliable signal. The name compare is a fallback
// for a card the scanner itself classified as Momentum's NfcProtocolEmv (id
// 14, not in the SDK enum this app compiles against) whose own PPSE select
// nonetheless failed; the string comes from the firmware's own device table
// (EMV_PROTOCOL_NAME "EMV", verified in Momentum lib/nfc/protocols/emv/emv.c).
bool reader_is_payment_card(const ReaderApp* app) {
    if(app->card != ReaderCardNfc) return false;
    if(app->emv.ppse_ok || app->emv.aid_count > 0) return true;
    const char* name = nfc_device_get_protocol_name(app->display_protocol);
    return name && strcmp(name, "EMV") == 0;
}

void reader_nfc_handle_scanned(ReaderApp* app) {
    // The scanner re-detects in a loop, so this can arrive more than once.
    // Only the first one may start a poller.
    if(!app->scanner || app->poller) return;

    furi_timer_stop(app->phase_timer);
    nfc_scanner_stop(app->scanner);
    nfc_scanner_free(app->scanner);
    app->scanner = NULL;

    // Retires any duplicate ReaderEventNfcScanned still in the queue.
    app->toolkit->gen++;

    reader_set_state(app, ReaderStateReading);
    // Restart every per-read state so a rescan begins cleanly.
    app->mfc_pass = 0;
    app->mfc_sector = 0;
    app->mfc_recovery_pending = false;
    memset(&app->emv, 0, sizeof(app->emv)); // no stale bank data from a previous card
    app->emv_reactivate = false; // first Ready → halt+reactivate to reset card app state
    app->poller = nfc_poller_alloc(app->nfc, app->poll_protocol);
    nfc_poller_start(app->poller, reader_poller_callback, app);
    // Bounded read: a card removed now must not leave us stuck on "Reading".
    // Longer bounds cover in-callback work (EMV APDU chain, Classic key
    // passes): nfc_poller_stop() joins the worker thread, so a short
    // timeout would block the GUI thread mid-read.
    furi_timer_start(
        app->phase_timer, furi_ms_to_ticks(reader_read_timeout_for(app->poll_protocol)));
}

void reader_nfc_handle_read(ReaderApp* app) {
    if(!app->poller) return;
    reader_stop_all(app);
    app->toolkit->gen++;
    app->card = ReaderCardNfc;

    // If the MfClassic poller finished but left sectors locked, run a
    // key-recovery pass before rendering. The recovery poller runs on the
    // GUI thread so it never overlaps with the read poller (which is already
    // stopped above).
    if(app->mfc_recovery_pending) {
        app->mfc_recovery_pending = false;
        const MfClassicData* partial =
            (const MfClassicData*)nfc_device_get_data(app->device, NfcProtocolMfClassic);
        if(partial) {
            MfClassicData* recovered = mf_classic_alloc();
            mf_classic_copy(recovered, partial);

            size_t uid_len = 0;
            const uint8_t* uid = mf_classic_get_uid(recovered, &uid_len);

            furi_timer_start(
                app->phase_timer, furi_ms_to_ticks(MFC_RECOVERY_TIMEOUT_MS));
            mfc_recover_keys(app->nfc, uid, uid_len, recovered);
            nfc_device_set_data(app->device, NfcProtocolMfClassic, recovered);
            mf_classic_free(recovered);
        }
    }

    reader_report_begin(app);
    card_info_format_nfc(app->info_text, app->device, app->display_protocol, &app->emv);
    if(reader_is_payment_card(app)) {
        furi_string_cat_str(
            app->info_text,
            "\n[Policy] Bank card: emulation disabled;\nsave stores UID/ATS only.\n");
    }
    reader_report_show(app, nfc_device_get_protocol_name(app->display_protocol));
}
