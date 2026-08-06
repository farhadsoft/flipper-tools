#pragma once

#include "reader_app.h"

// The most-derived protocol we can safely poll/emulate for `p` — walks up
// to an ancestor in the compile-time whitelist (ids 0..11) via
// nfc_protocol_has_parent(), never touching fork-sensitive sentinels. Used
// both for a live scan result and (by reader_do_load()) for a protocol
// loaded from a .nfc file, so poll_protocol always satisfies the same
// invariant regardless of where the card data came from.
NfcProtocol reader_poll_protocol(NfcProtocol p);
bool reader_protocol_emulatable(NfcProtocol p);

void reader_stop_nfc(ReaderApp* app);
void reader_start_nfc_phase(ReaderApp* app);
void reader_start_nfc_emulation(ReaderApp* app);

bool reader_is_payment_card(const ReaderApp* app);

// Handlers for ReaderEventNfcScanned / ReaderEventNfcRead. GUI thread only
// (reader_nfc_handle_scanned() starts the poller; reader_nfc_handle_read()
// stops the radio via reader_stop_all()).
void reader_nfc_handle_scanned(ReaderApp* app);
void reader_nfc_handle_read(ReaderApp* app);
